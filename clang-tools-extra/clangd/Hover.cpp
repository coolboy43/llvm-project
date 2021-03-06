//===--- Hover.cpp - Information about code at the cursor location --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Hover.h"

#include "AST.h"
#include "CodeCompletionStrings.h"
#include "FindTarget.h"
#include "FormattedString.h"
#include "Logger.h"
#include "ParsedAST.h"
#include "Selection.h"
#include "SourceCode.h"
#include "index/SymbolCollector.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Index/IndexSymbol.h"
#include "clang/Tooling/Syntax/Tokens.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <string>

namespace clang {
namespace clangd {
namespace {

PrintingPolicy printingPolicyForDecls(PrintingPolicy Base) {
  PrintingPolicy Policy(Base);

  Policy.AnonymousTagLocations = false;
  Policy.TerseOutput = true;
  Policy.PolishForDeclaration = true;
  Policy.ConstantsAsWritten = true;
  Policy.SuppressTagKeyword = false;

  return Policy;
}

/// Given a declaration \p D, return a human-readable string representing the
/// local scope in which it is declared, i.e. class(es) and method name. Returns
/// an empty string if it is not local.
std::string getLocalScope(const Decl *D) {
  std::vector<std::string> Scopes;
  const DeclContext *DC = D->getDeclContext();
  auto GetName = [](const TypeDecl *D) {
    if (!D->getDeclName().isEmpty()) {
      PrintingPolicy Policy = D->getASTContext().getPrintingPolicy();
      Policy.SuppressScope = true;
      return declaredType(D).getAsString(Policy);
    }
    if (auto RD = dyn_cast<RecordDecl>(D))
      return ("(anonymous " + RD->getKindName() + ")").str();
    return std::string("");
  };
  while (DC) {
    if (const TypeDecl *TD = dyn_cast<TypeDecl>(DC))
      Scopes.push_back(GetName(TD));
    else if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(DC))
      Scopes.push_back(FD->getNameAsString());
    DC = DC->getParent();
  }

  return llvm::join(llvm::reverse(Scopes), "::");
}

/// Returns the human-readable representation for namespace containing the
/// declaration \p D. Returns empty if it is contained global namespace.
std::string getNamespaceScope(const Decl *D) {
  const DeclContext *DC = D->getDeclContext();

  if (const TagDecl *TD = dyn_cast<TagDecl>(DC))
    return getNamespaceScope(TD);
  if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(DC))
    return getNamespaceScope(FD);
  if (const NamespaceDecl *NSD = dyn_cast<NamespaceDecl>(DC)) {
    // Skip inline/anon namespaces.
    if (NSD->isInline() || NSD->isAnonymousNamespace())
      return getNamespaceScope(NSD);
  }
  if (const NamedDecl *ND = dyn_cast<NamedDecl>(DC))
    return printQualifiedName(*ND);

  return "";
}

std::string printDefinition(const Decl *D) {
  std::string Definition;
  llvm::raw_string_ostream OS(Definition);
  PrintingPolicy Policy =
      printingPolicyForDecls(D->getASTContext().getPrintingPolicy());
  Policy.IncludeTagDefinition = false;
  Policy.SuppressTemplateArgsInCXXConstructors = true;
  Policy.SuppressTagKeyword = true;
  D->print(OS, Policy);
  OS.flush();
  return Definition;
}

std::string printType(QualType QT, const PrintingPolicy &Policy) {
  // TypePrinter doesn't resolve decltypes, so resolve them here.
  // FIXME: This doesn't handle composite types that contain a decltype in them.
  // We should rather have a printing policy for that.
  while (const auto *DT = QT->getAs<DecltypeType>())
    QT = DT->getUnderlyingType();
  return QT.getAsString(Policy);
}

std::string printType(const TemplateTypeParmDecl *TTP) {
  std::string Res = TTP->wasDeclaredWithTypename() ? "typename" : "class";
  if (TTP->isParameterPack())
    Res += "...";
  return Res;
}

std::string printType(const NonTypeTemplateParmDecl *NTTP,
                      const PrintingPolicy &PP) {
  std::string Res = printType(NTTP->getType(), PP);
  if (NTTP->isParameterPack())
    Res += "...";
  return Res;
}

std::string printType(const TemplateTemplateParmDecl *TTP,
                      const PrintingPolicy &PP) {
  std::string Res;
  llvm::raw_string_ostream OS(Res);
  OS << "template <";
  llvm::StringRef Sep = "";
  for (const Decl *Param : *TTP->getTemplateParameters()) {
    OS << Sep;
    Sep = ", ";
    if (const auto *TTP = dyn_cast<TemplateTypeParmDecl>(Param))
      OS << printType(TTP);
    else if (const auto *NTTP = dyn_cast<NonTypeTemplateParmDecl>(Param))
      OS << printType(NTTP, PP);
    else if (const auto *TTPD = dyn_cast<TemplateTemplateParmDecl>(Param))
      OS << printType(TTPD, PP);
  }
  // FIXME: TemplateTemplateParameter doesn't store the info on whether this
  // param was a "typename" or "class".
  OS << "> class";
  return OS.str();
}

std::vector<HoverInfo::Param>
fetchTemplateParameters(const TemplateParameterList *Params,
                        const PrintingPolicy &PP) {
  assert(Params);
  std::vector<HoverInfo::Param> TempParameters;

  for (const Decl *Param : *Params) {
    HoverInfo::Param P;
    if (const auto *TTP = dyn_cast<TemplateTypeParmDecl>(Param)) {
      P.Type = printType(TTP);

      if (!TTP->getName().empty())
        P.Name = TTP->getNameAsString();

      if (TTP->hasDefaultArgument())
        P.Default = TTP->getDefaultArgument().getAsString(PP);
    } else if (const auto *NTTP = dyn_cast<NonTypeTemplateParmDecl>(Param)) {
      P.Type = printType(NTTP, PP);

      if (IdentifierInfo *II = NTTP->getIdentifier())
        P.Name = II->getName().str();

      if (NTTP->hasDefaultArgument()) {
        P.Default.emplace();
        llvm::raw_string_ostream Out(*P.Default);
        NTTP->getDefaultArgument()->printPretty(Out, nullptr, PP);
      }
    } else if (const auto *TTPD = dyn_cast<TemplateTemplateParmDecl>(Param)) {
      P.Type = printType(TTPD, PP);

      if (!TTPD->getName().empty())
        P.Name = TTPD->getNameAsString();

      if (TTPD->hasDefaultArgument()) {
        P.Default.emplace();
        llvm::raw_string_ostream Out(*P.Default);
        TTPD->getDefaultArgument().getArgument().print(PP, Out);
      }
    }
    TempParameters.push_back(std::move(P));
  }

  return TempParameters;
}

const FunctionDecl *getUnderlyingFunction(const Decl *D) {
  // Extract lambda from variables.
  if (const VarDecl *VD = llvm::dyn_cast<VarDecl>(D)) {
    auto QT = VD->getType();
    if (!QT.isNull()) {
      while (!QT->getPointeeType().isNull())
        QT = QT->getPointeeType();

      if (const auto *CD = QT->getAsCXXRecordDecl())
        return CD->getLambdaCallOperator();
    }
  }

  // Non-lambda functions.
  return D->getAsFunction();
}

// Returns the decl that should be used for querying comments, either from index
// or AST.
const NamedDecl *getDeclForComment(const NamedDecl *D) {
  if (const auto *TSD = llvm::dyn_cast<ClassTemplateSpecializationDecl>(D)) {
    // Template may not be instantiated e.g. if the type didn't need to be
    // complete; fallback to primary template.
    if (TSD->getTemplateSpecializationKind() == TSK_Undeclared)
      return TSD->getSpecializedTemplate();
    if (const auto *TIP = TSD->getTemplateInstantiationPattern())
      return TIP;
  }
  if (const auto *TSD = llvm::dyn_cast<VarTemplateSpecializationDecl>(D)) {
    if (TSD->getTemplateSpecializationKind() == TSK_Undeclared)
      return TSD->getSpecializedTemplate();
    if (const auto *TIP = TSD->getTemplateInstantiationPattern())
      return TIP;
  }
  if (const auto *FD = D->getAsFunction())
    if (const auto *TIP = FD->getTemplateInstantiationPattern())
      return TIP;
  return D;
}

// Look up information about D from the index, and add it to Hover.
void enhanceFromIndex(HoverInfo &Hover, const NamedDecl &ND,
                      const SymbolIndex *Index) {
  assert(&ND == getDeclForComment(&ND));
  // We only add documentation, so don't bother if we already have some.
  if (!Hover.Documentation.empty() || !Index)
    return;

  // Skip querying for non-indexable symbols, there's no point.
  // We're searching for symbols that might be indexed outside this main file.
  if (!SymbolCollector::shouldCollectSymbol(ND, ND.getASTContext(),
                                            SymbolCollector::Options(),
                                            /*IsMainFileOnly=*/false))
    return;
  auto ID = getSymbolID(&ND);
  if (!ID)
    return;
  LookupRequest Req;
  Req.IDs.insert(*ID);
  Index->lookup(Req, [&](const Symbol &S) {
    Hover.Documentation = std::string(S.Documentation);
  });
}

// Default argument might exist but be unavailable, in the case of unparsed
// arguments for example. This function returns the default argument if it is
// available.
const Expr *getDefaultArg(const ParmVarDecl *PVD) {
  // Default argument can be unparsed or uninstatiated. For the former we
  // can't do much, as token information is only stored in Sema and not
  // attached to the AST node. For the latter though, it is safe to proceed as
  // the expression is still valid.
  if (!PVD->hasDefaultArg() || PVD->hasUnparsedDefaultArg())
    return nullptr;
  return PVD->hasUninstantiatedDefaultArg() ? PVD->getUninstantiatedDefaultArg()
                                            : PVD->getDefaultArg();
}

// Populates Type, ReturnType, and Parameters for function-like decls.
void fillFunctionTypeAndParams(HoverInfo &HI, const Decl *D,
                               const FunctionDecl *FD,
                               const PrintingPolicy &Policy) {
  HI.Parameters.emplace();
  for (const ParmVarDecl *PVD : FD->parameters()) {
    HI.Parameters->emplace_back();
    auto &P = HI.Parameters->back();
    if (!PVD->getType().isNull()) {
      P.Type = printType(PVD->getType(), Policy);
    } else {
      std::string Param;
      llvm::raw_string_ostream OS(Param);
      PVD->dump(OS);
      OS.flush();
      elog("Got param with null type: {0}", Param);
    }
    if (!PVD->getName().empty())
      P.Name = PVD->getNameAsString();
    if (const Expr *DefArg = getDefaultArg(PVD)) {
      P.Default.emplace();
      llvm::raw_string_ostream Out(*P.Default);
      DefArg->printPretty(Out, nullptr, Policy);
    }
  }

  // We don't want any type info, if name already contains it. This is true for
  // constructors/destructors and conversion operators.
  const auto NK = FD->getDeclName().getNameKind();
  if (NK == DeclarationName::CXXConstructorName ||
      NK == DeclarationName::CXXDestructorName ||
      NK == DeclarationName::CXXConversionFunctionName)
    return;

  HI.ReturnType = printType(FD->getReturnType(), Policy);
  QualType QT = FD->getType();
  if (const VarDecl *VD = llvm::dyn_cast<VarDecl>(D)) // Lambdas
    QT = VD->getType().getDesugaredType(D->getASTContext());
  HI.Type = printType(QT, Policy);
  // FIXME: handle variadics.
}

llvm::Optional<std::string> printExprValue(const Expr *E,
                                           const ASTContext &Ctx) {
  Expr::EvalResult Constant;
  // Evaluating [[foo]]() as "&foo" isn't useful, and prevents us walking up
  // to the enclosing call.
  QualType T = E->getType();
  if (T.isNull() || T->isFunctionType() || T->isFunctionPointerType() ||
      T->isFunctionReferenceType())
    return llvm::None;
  // Attempt to evaluate. If expr is dependent, evaluation crashes!
  if (E->isValueDependent() || !E->EvaluateAsRValue(Constant, Ctx))
    return llvm::None;

  // Show enums symbolically, not numerically like APValue::printPretty().
  if (T->isEnumeralType() && Constant.Val.getInt().getMinSignedBits() <= 64) {
    // Compare to int64_t to avoid bit-width match requirements.
    int64_t Val = Constant.Val.getInt().getExtValue();
    for (const EnumConstantDecl *ECD :
         T->castAs<EnumType>()->getDecl()->enumerators())
      if (ECD->getInitVal() == Val)
        return llvm::formatv("{0} ({1})", ECD->getNameAsString(), Val).str();
  }
  return Constant.Val.getAsString(Ctx, E->getType());
}

llvm::Optional<std::string> printExprValue(const SelectionTree::Node *N,
                                           const ASTContext &Ctx) {
  for (; N; N = N->Parent) {
    // Try to evaluate the first evaluatable enclosing expression.
    if (const Expr *E = N->ASTNode.get<Expr>()) {
      if (auto Val = printExprValue(E, Ctx))
        return Val;
    } else if (N->ASTNode.get<Decl>() || N->ASTNode.get<Stmt>()) {
      // Refuse to cross certain non-exprs. (TypeLoc are OK as part of Exprs).
      // This tries to ensure we're showing a value related to the cursor.
      break;
    }
  }
  return llvm::None;
}

/// Generate a \p Hover object given the declaration \p D.
HoverInfo getHoverContents(const NamedDecl *D, const SymbolIndex *Index) {
  HoverInfo HI;
  const ASTContext &Ctx = D->getASTContext();

  HI.NamespaceScope = getNamespaceScope(D);
  if (!HI.NamespaceScope->empty())
    HI.NamespaceScope->append("::");
  HI.LocalScope = getLocalScope(D);
  if (!HI.LocalScope.empty())
    HI.LocalScope.append("::");

  PrintingPolicy Policy = printingPolicyForDecls(Ctx.getPrintingPolicy());
  HI.Name = printName(Ctx, *D);
  const auto *CommentD = getDeclForComment(D);
  HI.Documentation = getDeclComment(Ctx, *CommentD);
  enhanceFromIndex(HI, *CommentD, Index);

  HI.Kind = index::getSymbolInfo(D).Kind;

  // Fill in template params.
  if (const TemplateDecl *TD = D->getDescribedTemplate()) {
    HI.TemplateParameters =
        fetchTemplateParameters(TD->getTemplateParameters(), Policy);
    D = TD;
  } else if (const FunctionDecl *FD = D->getAsFunction()) {
    if (const auto *FTD = FD->getDescribedTemplate()) {
      HI.TemplateParameters =
          fetchTemplateParameters(FTD->getTemplateParameters(), Policy);
      D = FTD;
    }
  }

  // Fill in types and params.
  if (const FunctionDecl *FD = getUnderlyingFunction(D))
    fillFunctionTypeAndParams(HI, D, FD, Policy);
  else if (const auto *VD = dyn_cast<ValueDecl>(D))
    HI.Type = printType(VD->getType(), Policy);
  else if (const auto *TTP = dyn_cast<TemplateTypeParmDecl>(D))
    HI.Type = TTP->wasDeclaredWithTypename() ? "typename" : "class";
  else if (const auto *TTP = dyn_cast<TemplateTemplateParmDecl>(D))
    HI.Type = printType(TTP, Policy);

  // Fill in value with evaluated initializer if possible.
  if (const auto *Var = dyn_cast<VarDecl>(D)) {
    if (const Expr *Init = Var->getInit())
      HI.Value = printExprValue(Init, Ctx);
  } else if (const auto *ECD = dyn_cast<EnumConstantDecl>(D)) {
    // Dependent enums (e.g. nested in template classes) don't have values yet.
    if (!ECD->getType()->isDependentType())
      HI.Value = ECD->getInitVal().toString(10);
  }

  HI.Definition = printDefinition(D);
  return HI;
}

/// Generate a \p Hover object given the type \p T.
HoverInfo getHoverContents(QualType T, ASTContext &ASTCtx,
                           const SymbolIndex *Index) {
  HoverInfo HI;

  if (const auto *D = T->getAsTagDecl()) {
    HI.Name = printName(ASTCtx, *D);
    HI.Kind = index::getSymbolInfo(D).Kind;

    const auto *CommentD = getDeclForComment(D);
    HI.Documentation = getDeclComment(ASTCtx, *CommentD);
    enhanceFromIndex(HI, *CommentD, Index);
  } else {
    // Builtin types
    auto Policy = printingPolicyForDecls(ASTCtx.getPrintingPolicy());
    Policy.SuppressTagKeyword = true;
    HI.Name = T.getAsString(Policy);
  }
  return HI;
}

/// Generate a \p Hover object given the macro \p MacroDecl.
HoverInfo getHoverContents(const DefinedMacro &Macro, ParsedAST &AST) {
  HoverInfo HI;
  SourceManager &SM = AST.getSourceManager();
  HI.Name = std::string(Macro.Name);
  HI.Kind = index::SymbolKind::Macro;
  // FIXME: Populate documentation
  // FIXME: Pupulate parameters

  // Try to get the full definition, not just the name
  SourceLocation StartLoc = Macro.Info->getDefinitionLoc();
  SourceLocation EndLoc = Macro.Info->getDefinitionEndLoc();
  if (EndLoc.isValid()) {
    EndLoc = Lexer::getLocForEndOfToken(EndLoc, 0, SM, AST.getLangOpts());
    bool Invalid;
    StringRef Buffer = SM.getBufferData(SM.getFileID(StartLoc), &Invalid);
    if (!Invalid) {
      unsigned StartOffset = SM.getFileOffset(StartLoc);
      unsigned EndOffset = SM.getFileOffset(EndLoc);
      if (EndOffset <= Buffer.size() && StartOffset < EndOffset)
        HI.Definition =
            ("#define " + Buffer.substr(StartOffset, EndOffset - StartOffset))
                .str();
    }
  }
  return HI;
}

bool isLiteral(const Expr *E) {
  // Unfortunately there's no common base Literal classes inherits from
  // (apart from Expr), therefore this is a nasty blacklist.
  return llvm::isa<CharacterLiteral>(E) || llvm::isa<CompoundLiteralExpr>(E) ||
         llvm::isa<CXXBoolLiteralExpr>(E) ||
         llvm::isa<CXXNullPtrLiteralExpr>(E) ||
         llvm::isa<FixedPointLiteral>(E) || llvm::isa<FloatingLiteral>(E) ||
         llvm::isa<ImaginaryLiteral>(E) || llvm::isa<IntegerLiteral>(E) ||
         llvm::isa<StringLiteral>(E) || llvm::isa<UserDefinedLiteral>(E);
}

llvm::StringLiteral getNameForExpr(const Expr *E) {
  // FIXME: Come up with names for `special` expressions.
  //
  // It's an known issue for GCC5, https://godbolt.org/z/Z_tbgi. Work around
  // that by using explicit conversion constructor.
  //
  // TODO: Once GCC5 is fully retired and not the minimal requirement as stated
  // in `GettingStarted`, please remove the explicit conversion constructor.
  return llvm::StringLiteral("expression");
}

// Generates hover info for evaluatable expressions.
// FIXME: Support hover for literals (esp user-defined)
llvm::Optional<HoverInfo> getHoverContents(const Expr *E, ParsedAST &AST) {
  // There's not much value in hovering over "42" and getting a hover card
  // saying "42 is an int", similar for other literals.
  if (isLiteral(E))
    return llvm::None;

  HoverInfo HI;
  // For expressions we currently print the type and the value, iff it is
  // evaluatable.
  if (auto Val = printExprValue(E, AST.getASTContext())) {
    auto Policy =
        printingPolicyForDecls(AST.getASTContext().getPrintingPolicy());
    Policy.SuppressTagKeyword = true;
    HI.Type = printType(E->getType(), Policy);
    HI.Value = *Val;
    HI.Name = std::string(getNameForExpr(E));
    return HI;
  }
  return llvm::None;
}

bool isParagraphLineBreak(llvm::StringRef Str, size_t LineBreakIndex) {
  return Str.substr(LineBreakIndex + 1)
      .drop_while([](auto C) { return C == ' ' || C == '\t'; })
      .startswith("\n");
};

bool isPunctuationLineBreak(llvm::StringRef Str, size_t LineBreakIndex) {
  constexpr llvm::StringLiteral Punctuation = R"txt(.:,;!?)txt";

  return LineBreakIndex > 0 && Punctuation.contains(Str[LineBreakIndex - 1]);
};

bool isFollowedByHardLineBreakIndicator(llvm::StringRef Str,
                                        size_t LineBreakIndex) {
  // '-'/'*' md list, '@'/'\' documentation command, '>' md blockquote,
  // '#' headings, '`' code blocks
  constexpr llvm::StringLiteral LinbreakIdenticators = R"txt(-*@\>#`)txt";

  auto NextNonSpaceCharIndex = Str.find_first_not_of(' ', LineBreakIndex + 1);

  if (NextNonSpaceCharIndex == llvm::StringRef::npos) {
    return false;
  }

  auto FollowedBySingleCharIndicator =
      LinbreakIdenticators.find(Str[NextNonSpaceCharIndex]) !=
      llvm::StringRef::npos;

  auto FollowedByNumberedListIndicator =
      llvm::isDigit(Str[NextNonSpaceCharIndex]) &&
      NextNonSpaceCharIndex + 1 < Str.size() &&
      (Str[NextNonSpaceCharIndex + 1] == '.' ||
       Str[NextNonSpaceCharIndex + 1] == ')');

  return FollowedBySingleCharIndicator || FollowedByNumberedListIndicator;
};

bool isHardLineBreak(llvm::StringRef Str, size_t LineBreakIndex) {
  return isPunctuationLineBreak(Str, LineBreakIndex) ||
         isFollowedByHardLineBreakIndicator(Str, LineBreakIndex);
}

} // namespace

llvm::Optional<HoverInfo> getHover(ParsedAST &AST, Position Pos,
                                   format::FormatStyle Style,
                                   const SymbolIndex *Index) {
  const SourceManager &SM = AST.getSourceManager();
  auto CurLoc = sourceLocationInMainFile(SM, Pos);
  if (!CurLoc) {
    llvm::consumeError(CurLoc.takeError());
    return llvm::None;
  }
  const auto &TB = AST.getTokens();
  auto TokensTouchingCursor = syntax::spelledTokensTouching(*CurLoc, TB);
  // Early exit if there were no tokens around the cursor.
  if (TokensTouchingCursor.empty())
    return llvm::None;

  // To be used as a backup for highlighting the selected token, we use back as
  // it aligns better with biases elsewhere (editors tend to send the position
  // for the left of the hovered token).
  CharSourceRange HighlightRange =
      TokensTouchingCursor.back().range(SM).toCharRange(SM);
  llvm::Optional<HoverInfo> HI;
  // Macros and deducedtype only works on identifiers and auto/decltype keywords
  // respectively. Therefore they are only trggered on whichever works for them,
  // similar to SelectionTree::create().
  for (const auto &Tok : TokensTouchingCursor) {
    if (Tok.kind() == tok::identifier) {
      // Prefer the identifier token as a fallback highlighting range.
      HighlightRange = Tok.range(SM).toCharRange(SM);
      if (auto M = locateMacroAt(Tok, AST.getPreprocessor())) {
        HI = getHoverContents(*M, AST);
        break;
      }
    } else if (Tok.kind() == tok::kw_auto || Tok.kind() == tok::kw_decltype) {
      if (auto Deduced = getDeducedType(AST.getASTContext(), Tok.location())) {
        HI = getHoverContents(*Deduced, AST.getASTContext(), Index);
        HighlightRange = Tok.range(SM).toCharRange(SM);
        break;
      }
    }
  }

  // If it wasn't auto/decltype or macro, look for decls and expressions.
  if (!HI) {
    auto Offset = SM.getFileOffset(*CurLoc);
    // Editors send the position on the left of the hovered character.
    // So our selection tree should be biased right. (Tested with VSCode).
    SelectionTree ST =
        SelectionTree::createRight(AST.getASTContext(), TB, Offset, Offset);
    std::vector<const Decl *> Result;
    if (const SelectionTree::Node *N = ST.commonAncestor()) {
      // FIXME: Fill in HighlightRange with range coming from N->ASTNode.
      auto Decls = explicitReferenceTargets(N->ASTNode, DeclRelation::Alias);
      if (!Decls.empty()) {
        HI = getHoverContents(Decls.front(), Index);
        // Look for a close enclosing expression to show the value of.
        if (!HI->Value)
          HI->Value = printExprValue(N, AST.getASTContext());
      } else if (const Expr *E = N->ASTNode.get<Expr>()) {
        HI = getHoverContents(E, AST);
      }
      // FIXME: support hovers for other nodes?
      //  - built-in types
    }
  }

  if (!HI)
    return llvm::None;

  auto Replacements = format::reformat(
      Style, HI->Definition, tooling::Range(0, HI->Definition.size()));
  if (auto Formatted =
          tooling::applyAllReplacements(HI->Definition, Replacements))
    HI->Definition = *Formatted;
  HI->SymRange = halfOpenToRange(SM, HighlightRange);

  return HI;
}

markup::Document HoverInfo::present() const {
  markup::Document Output;
  // Header contains a text of the form:
  // variable `var`
  //
  // class `X`
  //
  // function `foo`
  //
  // expression
  //
  // Note that we are making use of a level-3 heading because VSCode renders
  // level 1 and 2 headers in a huge font, see
  // https://github.com/microsoft/vscode/issues/88417 for details.
  markup::Paragraph &Header = Output.addHeading(3);
  if (Kind != index::SymbolKind::Unknown)
    Header.appendText(std::string(index::getSymbolKindString(Kind)));
  assert(!Name.empty() && "hover triggered on a nameless symbol");
  Header.appendCode(Name);

  // Put a linebreak after header to increase readability.
  Output.addRuler();
  // Print Types on their own lines to reduce chances of getting line-wrapped by
  // editor, as they might be long.
  if (ReturnType) {
    // For functions we display signature in a list form, e.g.:
    // → `x`
    // Parameters:
    // - `bool param1`
    // - `int param2 = 5`
    Output.addParagraph().appendText("→").appendCode(*ReturnType);
    if (Parameters && !Parameters->empty()) {
      Output.addParagraph().appendText("Parameters:");
      markup::BulletList &L = Output.addBulletList();
      for (const auto &Param : *Parameters) {
        std::string Buffer;
        llvm::raw_string_ostream OS(Buffer);
        OS << Param;
        L.addItem().addParagraph().appendCode(std::move(OS.str()));
      }
    }
  } else if (Type) {
    Output.addParagraph().appendText("Type: ").appendCode(*Type);
  }

  if (Value) {
    markup::Paragraph &P = Output.addParagraph();
    P.appendText("Value =");
    P.appendCode(*Value);
  }

  if (!Documentation.empty())
    parseDocumentation(Documentation, Output);

  if (!Definition.empty()) {
    Output.addRuler();
    std::string ScopeComment;
    // Drop trailing "::".
    if (!LocalScope.empty()) {
      // Container name, e.g. class, method, function.
      // We might want to propogate some info about container type to print
      // function foo, class X, method X::bar, etc.
      ScopeComment =
          "// In " + llvm::StringRef(LocalScope).rtrim(':').str() + '\n';
    } else if (NamespaceScope && !NamespaceScope->empty()) {
      ScopeComment = "// In namespace " +
                     llvm::StringRef(*NamespaceScope).rtrim(':').str() + '\n';
    }
    // Note that we don't print anything for global namespace, to not annoy
    // non-c++ projects or projects that are not making use of namespaces.
    Output.addCodeBlock(ScopeComment + Definition);
  }
  return Output;
}

void parseDocumentation(llvm::StringRef Input, markup::Document &Output) {

  constexpr auto WhiteSpaceChars = "\t\n\v\f\r ";

  auto TrimmedInput = Input.trim();

  std::string CurrentLine;

  for (size_t CharIndex = 0; CharIndex < TrimmedInput.size();) {
    if (TrimmedInput[CharIndex] == '\n') {
      // Trim whitespace infront of linebreak
      const auto LastNonSpaceCharIndex =
          CurrentLine.find_last_not_of(WhiteSpaceChars) + 1;
      CurrentLine.erase(LastNonSpaceCharIndex);

      if (isParagraphLineBreak(TrimmedInput, CharIndex) ||
          isHardLineBreak(TrimmedInput, CharIndex)) {
        // FIXME: maybe distinguish between line breaks and paragraphs
        Output.addParagraph().appendText(CurrentLine);
        CurrentLine = "";
      } else {
        // Ommit linebreak
        CurrentLine += ' ';
      }

      CharIndex++;
      // After a linebreak always remove spaces to avoid 4 space markdown code
      // blocks, also skip all additional linebreaks since they have no effect
      CharIndex = TrimmedInput.find_first_not_of(WhiteSpaceChars, CharIndex);
    } else {
      CurrentLine += TrimmedInput[CharIndex];
      CharIndex++;
    }
  }
  if (!CurrentLine.empty()) {
    Output.addParagraph().appendText(CurrentLine);
  }
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              const HoverInfo::Param &P) {
  std::vector<llvm::StringRef> Output;
  if (P.Type)
    Output.push_back(*P.Type);
  if (P.Name)
    Output.push_back(*P.Name);
  OS << llvm::join(Output, " ");
  if (P.Default)
    OS << " = " << *P.Default;
  return OS;
}

} // namespace clangd
} // namespace clang
