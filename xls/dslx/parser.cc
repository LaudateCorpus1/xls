// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/dslx/parser.h"

#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/types/variant.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/dslx/ast.h"
#include "xls/dslx/bindings.h"
#include "xls/dslx/builtins_metadata.h"
#include "xls/dslx/scanner.h"

namespace xls::dslx {

template <int N, typename... Ts>
struct GetNth {
  using type = typename std::tuple_element<N, std::tuple<Ts...>>::type;
};

template <int N, typename... ToTypes, typename... FromTypes>
absl::variant<ToTypes...> TryWidenVariant(
    const absl::variant<FromTypes...>& v) {
  using TryT = typename GetNth<N, FromTypes...>::type;
  if (absl::holds_alternative<TryT>(v)) {
    return absl::get<TryT>(v);
  }
  if constexpr (N == 0) {
    XLS_LOG(FATAL) << "Could not find variant in FromTypes.";
  } else {
    return TryWidenVariant<N - 1, ToTypes...>(v);
  }
}

template <typename... ToTypes, typename... FromTypes>
absl::variant<ToTypes...> WidenVariant(const absl::variant<FromTypes...>& v) {
  return TryWidenVariant<sizeof...(FromTypes) - 1, ToTypes...>(v);
}

template <int N, typename... ToTypes, typename... FromTypes>
absl::variant<ToTypes...> TryNarrowVariant(
    const absl::variant<FromTypes...>& v) {
  using TryT = typename GetNth<N, ToTypes...>::type;
  if (absl::holds_alternative<TryT>(v)) {
    return absl::get<TryT>(v);
  }
  if constexpr (N == 0) {
    XLS_LOG(FATAL) << "Could not find variant in ToTypes.";
  } else {
    return TryNarrowVariant<N - 1, ToTypes...>(v);
  }
}

template <typename... ToTypes, typename... FromTypes>
absl::variant<ToTypes...> NarrowVariant(const absl::variant<FromTypes...>& v) {
  return TryNarrowVariant<sizeof...(ToTypes) - 1, ToTypes...>(v);
}

template <typename... FromTypes>
NameDefTree::Leaf WidenToNameDefTreeLeaf(const absl::variant<FromTypes...>& v) {
  return WidenVariant<NameDef*, NameRef*, WildcardPattern*, Number*, ColonRef*>(
      v);
}

absl::StatusOr<BuiltinType> Parser::TokenToBuiltinType(const Token& tok) {
  return BuiltinTypeFromString(*tok.GetValue());
}

absl::StatusOr<Function*> Parser::ParseFunction(
    bool is_public, Bindings* bindings,
    absl::flat_hash_map<std::string, Function*>* name_to_fn) {
  XLS_RET_CHECK(bindings != nullptr);
  XLS_ASSIGN_OR_RETURN(Function * f,
                       ParseFunctionInternal(is_public, bindings));
  if (name_to_fn == nullptr) {
    return f;
  }
  auto [item, inserted] = name_to_fn->insert({f->identifier(), f});
  if (!inserted) {
    return ParseErrorStatus(
        f->name_def()->span(),
        absl::StrFormat("Function '%s' is defined in this module multiple "
                        "times; previously @ %s'",
                        f->identifier(), item->second->span().ToString()));
  }
  return f;
}

absl::StatusOr<std::unique_ptr<Module>> Parser::ParseModule(
    Bindings* bindings) {
  absl::optional<Bindings> stack_bindings;
  if (bindings == nullptr) {
    stack_bindings.emplace();
    bindings = &*stack_bindings;
  }

  for (auto const& it : GetParametricBuiltins()) {
    bindings->Add(std::string(it.first),
                  module_->Make<BuiltinNameDef>(std::string(it.first)));
  }

  absl::flat_hash_map<std::string, Function*> name_to_fn;

  while (!AtEof()) {
    XLS_ASSIGN_OR_RETURN(bool peek_is_eof, PeekTokenIs(TokenKind::kEof));
    if (peek_is_eof) {
      break;
    }

    XLS_ASSIGN_OR_RETURN(bool dropped_pub, TryDropKeyword(Keyword::kPub));
    if (dropped_pub) {
      XLS_ASSIGN_OR_RETURN(const Token* peek, PeekToken());
      if (peek->IsKeyword(Keyword::kFn)) {
        XLS_ASSIGN_OR_RETURN(Function * fn,
                             ParseFunction(
                                 /*is_public=*/true, bindings, &name_to_fn));
        module_->AddTop(fn);
        continue;
      } else if (peek->IsKeyword(Keyword::kProc)) {
        XLS_ASSIGN_OR_RETURN(Proc * proc, ParseProc(
                                              /*is_public=*/true, bindings));
        module_->AddTop(proc);
        continue;
      } else if (peek->IsKeyword(Keyword::kStruct)) {
        XLS_ASSIGN_OR_RETURN(StructDef * struct_def,
                             ParseStruct(/*is_public=*/true, bindings));
        module_->AddTop(struct_def);
        continue;
      } else if (peek->IsKeyword(Keyword::kEnum)) {
        XLS_ASSIGN_OR_RETURN(EnumDef * enum_def,
                             ParseEnumDef(/*is_public=*/true, bindings));
        module_->AddTop(enum_def);
        continue;
      } else if (peek->IsKeyword(Keyword::kConst)) {
        XLS_ASSIGN_OR_RETURN(ConstantDef * def,
                             ParseConstantDef(/*is_public=*/true, bindings));
        module_->AddTop(def);
        continue;
      } else if (peek->IsKeyword(Keyword::kType)) {
        XLS_ASSIGN_OR_RETURN(TypeDef * type_def,
                             ParseTypeDefinition(/*is_public=*/true, bindings));
        module_->AddTop(type_def);
        continue;
      }
      // TODO(leary): 2020-09-11 Also support `pub const`.
      return ParseErrorStatus(peek->span(),
                              "Expect a function, proc, struct, enum, or type "
                              "after 'pub' keyword.");
    }

    XLS_ASSIGN_OR_RETURN(bool dropped_hash, TryDropToken(TokenKind::kHash));
    if (dropped_hash) {
      XLS_ASSIGN_OR_RETURN(auto directive,
                           ParseDirective(&name_to_fn, bindings));
      if (auto* t = TryGet<TestFunction*>(directive)) {
        module_->AddTop(t);
      } else if (auto* tp = TryGet<TestProc*>(directive)) {
        module_->AddTop(tp);
      } else if (auto* qc = TryGet<QuickCheck*>(directive)) {
        module_->AddTop(qc);
      } else {
        // Nothing, was a directive for the parser.
      }
      continue;
    }

    XLS_ASSIGN_OR_RETURN(const Token* peek, PeekToken());
    auto top_level_error = [peek] {
      return ParseErrorStatus(
          peek->span(),
          absl::StrFormat("Expected start of top-level construct; got: %s'",
                          peek->ToString()));
    };
    if (peek->kind() != TokenKind::kKeyword) {
      return top_level_error();
    }

    switch (peek->GetKeyword()) {
      case Keyword::kFn: {
        XLS_ASSIGN_OR_RETURN(Function * fn,
                             ParseFunction(
                                 /*is_public=*/false, bindings, &name_to_fn));
        module_->AddTop(fn);
        break;
      }
      case Keyword::kProc: {
        XLS_ASSIGN_OR_RETURN(Proc * proc, ParseProc(
                                              /*is_public=*/false, bindings));
        module_->AddTop(proc);
        break;
      }
      case Keyword::kImport: {
        XLS_ASSIGN_OR_RETURN(Import * import, ParseImport(bindings));
        module_->AddTop(import);
        break;
      }
      case Keyword::kType: {
        XLS_ASSIGN_OR_RETURN(
            TypeDef * type_def,
            ParseTypeDefinition(/*is_public=*/false, bindings));
        module_->AddTop(type_def);
        break;
      }
      case Keyword::kStruct: {
        XLS_ASSIGN_OR_RETURN(StructDef * struct_,
                             ParseStruct(/*is_public=*/false, bindings));
        module_->AddTop(struct_);
        break;
      }
      case Keyword::kEnum: {
        XLS_ASSIGN_OR_RETURN(EnumDef * enum_,
                             ParseEnumDef(/*is_public=*/false, bindings));
        module_->AddTop(enum_);
        break;
      }
      case Keyword::kConst: {
        XLS_ASSIGN_OR_RETURN(ConstantDef * const_def,
                             ParseConstantDef(/*is_public=*/false, bindings));
        module_->AddTop(const_def);
        break;
      }
      default:
        return top_level_error();
    }
  }

  return std::move(module_);
}

absl::StatusOr<absl::variant<TestFunction*, TestProc*, QuickCheck*, nullptr_t>>
Parser::ParseDirective(absl::flat_hash_map<std::string, Function*>* name_to_fn,
                       Bindings* bindings) {
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kBang));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOBrack));
  XLS_ASSIGN_OR_RETURN(Token directive_tok,
                       PopTokenOrError(TokenKind::kIdentifier));
  const std::string& directive_name = directive_tok.GetStringValue();

  if (directive_name == "cfg") {
    XLS_RETURN_IF_ERROR(ParseConfig(directive_tok.span()));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCBrack));
    return nullptr;
  }
  if (directive_name == "test") {
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCBrack));
    XLS_ASSIGN_OR_RETURN(const Token* peek, PeekToken());
    if (peek->IsKeyword(Keyword::kFn)) {
      return ParseTestFunction(bindings, directive_tok.span());
    } else {
      return ParseErrorStatus(
          peek->span(), absl::StrCat("Invalid test type: ", peek->ToString()));
    }
  }
  if (directive_name == "test_proc") {
    std::vector<Expr*> initial_values;
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOParen));
    auto parse_term = [this, bindings] { return ParseTerm(bindings); };
    XLS_ASSIGN_OR_RETURN(initial_values,
                         ParseCommaSeq<Expr*>(parse_term, TokenKind::kCParen));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCBrack));
    return ParseTestProc(bindings, initial_values);
  }
  if (directive_name == "quickcheck") {
    XLS_ASSIGN_OR_RETURN(QuickCheck * n, ParseQuickCheck(name_to_fn, bindings,
                                                         directive_tok.span()));
    return n;
  }
  return ParseErrorStatus(
      directive_tok.span(),
      absl::StrFormat("Unknown directive: '%s'", directive_name));
}

absl::StatusOr<Expr*> Parser::ParseExpression(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(const Token* peek, PeekToken());
  if (peek->IsKeyword(Keyword::kLet) || peek->IsKeyword(Keyword::kConst)) {
    return ParseLet(bindings);
  }
  if (peek->IsKeyword(Keyword::kFor)) {
    return ParseFor(bindings);
  }
  if (peek->IsKeyword(Keyword::kChannel)) {
    return ParseChannelDecl(bindings);
  }
  if (peek->IsKeyword(Keyword::kSpawn)) {
    return ParseSpawn(bindings);
  }
  return ParseTernaryExpression(bindings);
}

absl::StatusOr<Expr*> Parser::ParseTernaryExpression(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(absl::optional<Token> if_, TryPopKeyword(Keyword::kIf));
  if (if_.has_value()) {  // Ternary
    XLS_ASSIGN_OR_RETURN(Expr * test, ParseExpression(bindings));
    XLS_VLOG(5) << "test: " << test->ToString();
    XLS_RETURN_IF_ERROR(
        DropTokenOrError(TokenKind::kOBrace, /*start=*/nullptr,
                         "Opening brace for 'if' (ternary) expression."));
    XLS_ASSIGN_OR_RETURN(Expr * consequent, ParseExpression(bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCBrace));
    XLS_RETURN_IF_ERROR(DropKeywordOrError(Keyword::kElse));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOBrace));
    XLS_ASSIGN_OR_RETURN(Expr * alternate, ParseExpression(bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCBrace));
    return module_->Make<Ternary>(Span(if_->span().start(), GetPos()), test,
                                  consequent, alternate);
  }
  return ParseLogicalOrExpression(bindings);
}

absl::StatusOr<TypeDef*> Parser::ParseTypeDefinition(bool is_public,
                                                     Bindings* bindings) {
  const Pos start_pos = GetPos();
  XLS_RETURN_IF_ERROR(DropKeywordOrError(Keyword::kType));
  XLS_ASSIGN_OR_RETURN(NameDef * name_def, ParseNameDef(bindings));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kEquals));
  XLS_ASSIGN_OR_RETURN(TypeAnnotation * type, ParseTypeAnnotation(bindings));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kSemi));
  Span span(start_pos, GetPos());
  auto* type_def = module_->Make<TypeDef>(span, name_def, type, is_public);
  name_def->set_definer(type_def);
  bindings->Add(name_def->identifier(), type_def);
  return type_def;
}

absl::StatusOr<Number*> Parser::TokenToNumber(const Token& tok) {
  NumberKind kind;
  switch (tok.kind()) {
    case TokenKind::kCharacter:
      kind = NumberKind::kCharacter;
      break;
    case TokenKind::kKeyword:
      kind = NumberKind::kBool;
      break;
    default:
      kind = NumberKind::kOther;
      break;
  }
  return module_->Make<Number>(tok.span(), *tok.GetValue(), kind,
                               /*type=*/nullptr);
}

absl::StatusOr<Expr*> Parser::ParseDim(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(const Token* peek, PeekToken());
  if (peek->kind() == TokenKind::kNumber) {
    return TokenToNumber(PopTokenOrDie());
  }
  XLS_ASSIGN_OR_RETURN(
      auto variant,
      ParseNameOrColonRef(bindings, "expected a valid dimension"));
  return ToExprNode(variant);
}

absl::StatusOr<StructRef> Parser::ResolveStruct(Bindings* bindings,
                                                TypeAnnotation* type) {
  auto type_ref_annotation = dynamic_cast<TypeRefTypeAnnotation*>(type);
  if (type_ref_annotation == nullptr) {
    return absl::InvalidArgumentError(
        "Can only resolve a TypeRefTypeAnnotation to a struct; got: " +
        type->ToString());
  }
  TypeRef* type_ref = type_ref_annotation->type_ref();
  TypeDefinition type_defn = type_ref->type_definition();

  if (absl::holds_alternative<StructDef*>(type_defn)) {
    return StructRef(absl::get<StructDef*>(type_defn));
  }
  if (absl::holds_alternative<ColonRef*>(type_defn)) {
    return StructRef(absl::get<ColonRef*>(type_defn));
  }
  if (absl::holds_alternative<TypeDef*>(type_defn)) {
    return ResolveStruct(bindings,
                         absl::get<TypeDef*>(type_defn)->type_annotation());
  }
  if (absl::holds_alternative<EnumDef*>(type_defn)) {
    return absl::InvalidArgumentError(
        "Type resolved to an enum definition; expected struct definition: " +
        type->ToString());
  }
  XLS_LOG(FATAL) << "Unhandled TypeDefinition variant.";
}

static absl::StatusOr<TypeDefinition> BoundNodeToTypeDefinition(BoundNode bn) {
  // clang-format off
  if (auto* e = TryGet<TypeDef*>(bn)) { return TypeDefinition(e); }
  if (auto* e = TryGet<StructDef*>(bn)) { return TypeDefinition(e); }
  if (auto* e = TryGet<EnumDef*>(bn)) { return TypeDefinition(e); }
  // clang-format on

  return absl::InvalidArgumentError("Could not convert to type definition: " +
                                    ToAstNode(bn)->ToString());
}

absl::StatusOr<TypeRef*> Parser::ParseTypeRef(Bindings* bindings,
                                              const Token& tok) {
  if (tok.kind() != TokenKind::kIdentifier) {
    return ParseErrorStatus(tok.span(), absl::StrFormat("Expected type; got %s",
                                                        tok.ToErrorString()));
  }

  XLS_ASSIGN_OR_RETURN(bool peek_is_double_colon,
                       PeekTokenIs(TokenKind::kDoubleColon));
  if (peek_is_double_colon) {
    return ParseModTypeRef(bindings, tok);
  }
  XLS_ASSIGN_OR_RETURN(BoundNode type_def, bindings->ResolveNodeOrError(
                                               *tok.GetValue(), tok.span()));
  if (!IsOneOf<TypeDef, EnumDef, StructDef>(ToAstNode(type_def))) {
    return ParseErrorStatus(
        tok.span(),
        absl::StrFormat(
            "Expected a type, but identifier '%s' doesn't resolve to "
            "a type, it resolved to a %s",
            *tok.GetValue(), BoundNodeGetTypeString(type_def)));
  }

  XLS_ASSIGN_OR_RETURN(TypeDefinition type_definition,
                       BoundNodeToTypeDefinition(type_def));
  return module_->Make<TypeRef>(tok.span(), *tok.GetValue(), type_definition);
}

absl::StatusOr<TypeAnnotation*> Parser::ParseTypeAnnotation(
    Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(Token tok, PopToken());

  if (tok.IsTypeKeyword()) {  // Builtin types.
    Pos start_pos = tok.span().start();
    if (tok.GetKeyword() == Keyword::kChannel) {
      XLS_ASSIGN_OR_RETURN(const Token* peek, PeekToken());
      if (peek->GetKeyword() == Keyword::kIn) {
        // Now get the type of the channels.
        XLS_RETURN_IF_ERROR(DropToken());
        XLS_ASSIGN_OR_RETURN(TypeAnnotation * payload,
                             ParseTypeAnnotation(bindings));
        return module_->Make<ChannelTypeAnnotation>(
            Span(start_pos, tok.span().limit()),
            ChannelTypeAnnotation::Direction::kIn, payload);
      } else if (peek->GetKeyword() == Keyword::kOut) {
        XLS_RETURN_IF_ERROR(DropToken());
        XLS_ASSIGN_OR_RETURN(TypeAnnotation * payload,
                             ParseTypeAnnotation(bindings));
        return module_->Make<ChannelTypeAnnotation>(
            Span(start_pos, tok.span().limit()),
            ChannelTypeAnnotation::Direction::kOut, payload);
      } else {
        return ParseErrorStatus(
            peek->span(),
            absl::StrFormat(
                "Expected a channel direction (\"in\" or \"out\"; got %s.",
                peek->GetStringValue()));
      }
    }

    Pos limit_pos = tok.span().limit();

    std::vector<Expr*> dims;
    XLS_ASSIGN_OR_RETURN(bool peek_is_obrack, PeekTokenIs(TokenKind::kOBrack));
    if (peek_is_obrack) {
      XLS_ASSIGN_OR_RETURN(dims, ParseDims(bindings, &limit_pos));
    }
    return MakeBuiltinTypeAnnotation(Span(start_pos, limit_pos), tok, dims);
  }

  if (tok.kind() == TokenKind::kOParen) {  // Tuple of types.
    auto parse_type_annotation = [this, bindings] {
      return ParseTypeAnnotation(bindings);
    };
    XLS_ASSIGN_OR_RETURN(std::vector<TypeAnnotation*> types,
                         ParseCommaSeq<TypeAnnotation*>(parse_type_annotation,
                                                        TokenKind::kCParen));

    Span span(tok.span().start(), GetPos());
    TypeAnnotation* type =
        module_->Make<TupleTypeAnnotation>(span, std::move(types));

    // Enable array of tuple type annotation.
    XLS_ASSIGN_OR_RETURN(bool peek_is_obrack, PeekTokenIs(TokenKind::kOBrack));
    if (peek_is_obrack) {
      XLS_ASSIGN_OR_RETURN(std::vector<Expr*> dims, ParseDims(bindings));
      for (Expr* dim : dims) {
        type = module_->Make<ArrayTypeAnnotation>(span, type, dim);
      }
    }
    return type;
  }

  // If the leader is not builtin and not a tuple, it's some form of type
  // reference.
  XLS_ASSIGN_OR_RETURN(TypeRef * type_ref, ParseTypeRef(bindings, tok));

  std::vector<Expr*> parametrics;
  XLS_ASSIGN_OR_RETURN(bool peek_is_oangle, PeekTokenIs(TokenKind::kOAngle));
  if (peek_is_oangle) {
    // Try to capture parametrics, if they're present. Capture in a transaction
    // so we can move on if they're not.
    auto status_or_parametrics = TryOrRollback<std::vector<Expr*>>(
        bindings,
        [this](Bindings* bindings) -> absl::StatusOr<std::vector<Expr*>> {
          return ParseParametrics(bindings);
        });
    if (status_or_parametrics.ok()) {
      parametrics = status_or_parametrics.value();
    }
  }

  std::vector<Expr*> dims;
  XLS_ASSIGN_OR_RETURN(bool peek_is_obrack, PeekTokenIs(TokenKind::kOBrack));
  if (peek_is_obrack) {  // Array type annotation.
    XLS_ASSIGN_OR_RETURN(dims, ParseDims(bindings));
  }

  Span span(tok.span().start(), GetPos());
  return MakeTypeRefTypeAnnotation(span, type_ref, std::move(dims),
                                   std::move(parametrics));
}

absl::StatusOr<NameRef*> Parser::ParseNameRef(Bindings* bindings,
                                              const Token* tok) {
  Transaction txn(this, bindings);
  auto cleanup = absl::MakeCleanup([&txn]() { txn.Rollback(); });

  absl::optional<Token> popped;
  if (tok == nullptr) {
    XLS_ASSIGN_OR_RETURN(popped, PopTokenOrError(TokenKind::kIdentifier));
    tok = &popped.value();
  }

  // If we failed to parse this ref, then put it back on the queue, in case
  // we try another production.
  XLS_ASSIGN_OR_RETURN(BoundNode bn, txn.bindings()->ResolveNodeOrError(
                                         *tok->GetValue(), tok->span()));
  AnyNameDef name_def = BoundNodeToAnyNameDef(bn);
  txn.CommitAndCancelCleanup(&cleanup);
  if (absl::holds_alternative<ConstantDef*>(bn)) {
    return module_->Make<ConstRef>(tok->span(), *tok->GetValue(), name_def);
  }
  return module_->Make<NameRef>(tok->span(), *tok->GetValue(), name_def);
}

absl::StatusOr<ColonRef*> Parser::ParseColonRef(Bindings* bindings,
                                                ColonRef::Subject subject) {
  Pos start = GetPos();
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kDoubleColon));
  while (true) {
    XLS_ASSIGN_OR_RETURN(Token value_tok,
                         PopTokenOrError(TokenKind::kIdentifier));
    Span span(start, GetPos());
    subject = module_->Make<ColonRef>(span, subject, *value_tok.GetValue());
    start = GetPos();
    XLS_ASSIGN_OR_RETURN(bool dropped_colon,
                         TryDropToken(TokenKind::kDoubleColon));
    if (dropped_colon) {
      continue;
    }
    return absl::get<ColonRef*>(subject);
  }
}

absl::StatusOr<Expr*> Parser::ParseCastOrEnumRefOrStructInstance(
    Bindings* bindings) {
  {
    // Put the first potential production in an isolated transaction; the other
    // productions below want this first token to remain in the stream.
    Transaction txn(this, bindings);
    auto cleanup = absl::MakeCleanup([&txn]() { txn.Rollback(); });
    Token tok = PopTokenOrDie();
    XLS_ASSIGN_OR_RETURN(bool peek_is_double_colon,
                         PeekTokenIs(TokenKind::kDoubleColon));
    if (peek_is_double_colon) {
      XLS_ASSIGN_OR_RETURN(NameRef * subject,
                           ParseNameRef(txn.bindings(), &tok));
      XLS_ASSIGN_OR_RETURN(ColonRef * ref,
                           ParseColonRef(txn.bindings(), subject));
      txn.CommitAndCancelCleanup(&cleanup);
      return ref;
    }
  }

  Transaction txn(this, bindings);
  auto cleanup = absl::MakeCleanup([&txn]() { txn.Rollback(); });
  XLS_ASSIGN_OR_RETURN(TypeAnnotation * type,
                       ParseTypeAnnotation(txn.bindings()));
  XLS_ASSIGN_OR_RETURN(bool peek_is_obrace, PeekTokenIs(TokenKind::kOBrace));
  Expr* expr;
  if (peek_is_obrace) {
    XLS_ASSIGN_OR_RETURN(expr, ParseStructInstance(txn.bindings(), type));
  } else {
    XLS_ASSIGN_OR_RETURN(expr, ParseCast(txn.bindings(), type));
  }
  txn.CommitAndCancelCleanup(&cleanup);
  return expr;
}

absl::StatusOr<Expr*> Parser::ParseStructInstance(Bindings* bindings,
                                                  TypeAnnotation* type) {
  XLS_VLOG(5) << "Parsing struct instance";
  if (type == nullptr) {
    XLS_ASSIGN_OR_RETURN(type, ParseTypeAnnotation(bindings));
  }

  const Pos start_pos = GetPos();

  XLS_ASSIGN_OR_RETURN(StructRef struct_ref, ResolveStruct(bindings, type));

  // TODO(https://github.com/google/xls/issues/247): If explicit parametrics
  // are present, then they should be matched with the StructDef's to verify
  // their types agree (a test should be written for this as well).
  (void)TryOrRollback<std::vector<Expr*>>(bindings, [this](Bindings* bindings) {
    return ParseParametrics(bindings);
  });

  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOBrace, nullptr,
                                       "Opening brace for struct instance."));

  using StructInstanceMember = std::pair<std::string, Expr*>;
  auto parse_struct_member =
      [this, bindings]() -> absl::StatusOr<StructInstanceMember> {
    XLS_ASSIGN_OR_RETURN(Token tok, PopTokenOrError(TokenKind::kIdentifier));
    XLS_ASSIGN_OR_RETURN(bool dropped_colon, TryDropToken(TokenKind::kColon));
    if (dropped_colon) {
      XLS_ASSIGN_OR_RETURN(Expr * e, ParseExpression(bindings));
      return std::make_pair(*tok.GetValue(), e);
    }

    XLS_ASSIGN_OR_RETURN(NameRef * name_ref, ParseNameRef(bindings, &tok));
    return std::make_pair(*tok.GetValue(), name_ref);
  };

  std::vector<StructInstanceMember> members;
  bool must_end = false;

  while (true) {
    XLS_ASSIGN_OR_RETURN(bool dropped_cbrace, TryDropToken(TokenKind::kCBrace));
    if (dropped_cbrace) {
      break;
    }
    if (must_end) {
      XLS_RETURN_IF_ERROR(DropTokenOrError(
          TokenKind::kCBrace, nullptr, "Closing brace for struct instance."));
      break;
    }
    XLS_ASSIGN_OR_RETURN(bool dropped_double_dot,
                         TryDropToken(TokenKind::kDoubleDot));
    if (dropped_double_dot) {
      XLS_ASSIGN_OR_RETURN(Expr * splatted, ParseExpression(bindings));
      XLS_RETURN_IF_ERROR(DropTokenOrError(
          TokenKind::kCBrace, nullptr,
          "Closing brace after struct instance \"splat\" (..) expression."));
      Span span(start_pos, GetPos());
      return module_->Make<SplatStructInstance>(span, struct_ref,
                                                std::move(members), splatted);
    }

    XLS_ASSIGN_OR_RETURN(StructInstanceMember member, parse_struct_member());
    members.push_back(std::move(member));
    XLS_ASSIGN_OR_RETURN(bool dropped_comma, TryDropToken(TokenKind::kComma));
    must_end = !dropped_comma;
  }
  Span span(start_pos, GetPos());
  return module_->Make<StructInstance>(span, struct_ref, std::move(members));
}

absl::StatusOr<absl::variant<NameRef*, ColonRef*>> Parser::ParseNameOrColonRef(
    Bindings* bindings, absl::string_view context) {
  XLS_ASSIGN_OR_RETURN(Token tok, PopTokenOrError(TokenKind::kIdentifier,
                                                  /*start=*/nullptr, context));
  XLS_ASSIGN_OR_RETURN(bool peek_is_double_colon,
                       PeekTokenIs(TokenKind::kDoubleColon));
  if (peek_is_double_colon) {
    XLS_ASSIGN_OR_RETURN(NameRef * subject, ParseNameRef(bindings, &tok));
    return ParseColonRef(bindings, subject);
  }
  return ParseNameRef(bindings, &tok);
}

absl::StatusOr<NameDef*> Parser::ParseNameDef(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(Token tok, PopTokenOrError(TokenKind::kIdentifier));
  XLS_ASSIGN_OR_RETURN(NameDef * name_def, TokenToNameDef(tok));
  bindings->Add(name_def->identifier(), name_def);
  return name_def;
}

absl::StatusOr<NameDefTree*> Parser::ParseNameDefTree(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(Token start, PopTokenOrError(TokenKind::kOParen));

  auto parse_name_def_or_tree = [bindings,
                                 this]() -> absl::StatusOr<NameDefTree*> {
    XLS_ASSIGN_OR_RETURN(bool peek_is_oparen, PeekTokenIs(TokenKind::kOParen));
    if (peek_is_oparen) {
      return ParseNameDefTree(bindings);
    }
    XLS_ASSIGN_OR_RETURN(auto name_def, ParseNameDefOrWildcard(bindings));
    return module_->Make<NameDefTree>(GetSpan(name_def),
                                      WidenToNameDefTreeLeaf(name_def));
  };

  XLS_ASSIGN_OR_RETURN(
      std::vector<NameDefTree*> branches,
      ParseCommaSeq<NameDefTree*>(parse_name_def_or_tree, TokenKind::kCParen));
  NameDefTree* ndt = module_->Make<NameDefTree>(
      Span(start.span().start(), GetPos()), std::move(branches));

  // Check that the name definitions are unique -- can't bind the same name
  // multiple times in one destructuring assignment.
  std::vector<NameDef*> name_defs = ndt->GetNameDefs();
  absl::flat_hash_map<absl::string_view, NameDef*> seen;
  for (NameDef* name_def : name_defs) {
    if (!seen.insert({name_def->identifier(), name_def}).second) {
      return ParseErrorStatus(
          name_def->span(),
          absl::StrFormat(
              "Name '%s' is defined twice in this pattern; previously @ %s",
              name_def->identifier(),
              seen[name_def->identifier()]->span().ToString()));
    }
  }
  return ndt;
}

absl::StatusOr<Array*> Parser::ParseArray(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(Token start_tok, PopTokenOrError(TokenKind::kOBrack));

  struct EllipsisSentinel {
    Span span;
  };

  using ExprOrEllipsis = absl::variant<Expr*, EllipsisSentinel>;
  auto parse_ellipsis_or_expression =
      [this, bindings]() -> absl::StatusOr<ExprOrEllipsis> {
    XLS_ASSIGN_OR_RETURN(bool peek_is_ellipsis,
                         PeekTokenIs(TokenKind::kEllipsis));
    if (peek_is_ellipsis) {
      Token tok = PopTokenOrDie();
      return EllipsisSentinel{tok.span()};
    }
    return ParseExpression(bindings);
  };
  auto get_span = [](const ExprOrEllipsis& e) {
    if (absl::holds_alternative<Expr*>(e)) {
      return absl::get<Expr*>(e)->span();
    }
    return absl::get<EllipsisSentinel>(e).span;
  };

  XLS_ASSIGN_OR_RETURN(std::vector<ExprOrEllipsis> members,
                       ParseCommaSeq<ExprOrEllipsis>(
                           parse_ellipsis_or_expression, {TokenKind::kCBrack}));
  std::vector<Expr*> exprs;
  bool has_trailing_ellipsis = false;
  for (int64_t i = 0; i < members.size(); ++i) {
    const ExprOrEllipsis& member = members[i];
    if (absl::holds_alternative<EllipsisSentinel>(member)) {
      if (i + 1 == members.size()) {
        has_trailing_ellipsis = true;
        members.pop_back();
      } else {
        return ParseErrorStatus(get_span(member),
                                "Ellipsis may only be in trailing position.");
      }
    } else {
      exprs.push_back(absl::get<Expr*>(member));
    }
  }

  Span span(start_tok.span().start(), GetPos());
  if (std::all_of(exprs.begin(), exprs.end(), IsConstant)) {
    return module_->Make<ConstantArray>(span, std::move(exprs),
                                        has_trailing_ellipsis);
  }
  return module_->Make<Array>(span, std::move(exprs), has_trailing_ellipsis);
}

absl::StatusOr<Expr*> Parser::ParseCast(Bindings* bindings,
                                        TypeAnnotation* type) {
  if (type == nullptr) {
    absl::StatusOr<TypeAnnotation*> type_status = ParseTypeAnnotation(bindings);
    if (type_status.status().ok()) {
      type = type_status.value();
    } else {
      PositionalErrorData data =
          GetPositionalErrorData(type_status.status()).value();
      return ParseErrorStatus(
          data.span,
          absl::StrFormat("Expected a type as part of a cast expression: %s",
                          data.message));
    }
  }

  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kColon));
  XLS_ASSIGN_OR_RETURN(Expr * term, ParseTerm(bindings));
  if (IsOneOf<Number, Array>(term)) {
    if (auto* n = dynamic_cast<Number*>(term)) {
      n->set_type_annotation(type);
    } else {
      auto* a = dynamic_cast<Array*>(term);
      a->set_type_annotation(type);
    }
    return term;
  }

  if (auto* tuple = dynamic_cast<XlsTuple*>(term);
      tuple != nullptr && std::all_of(tuple->members().begin(),
                                      tuple->members().end(), IsConstant)) {
    return term;
  }
  return ParseErrorStatus(
      type->span(),
      "Old-style cast only permitted for constant arrays/tuples "
      "and literal numbers.");
}

absl::StatusOr<Expr*> Parser::ParseBinopChain(
    const std::function<absl::StatusOr<Expr*>()>& sub_production,
    absl::variant<absl::Span<TokenKind const>, absl::Span<Keyword const>>
        target_tokens) {
  XLS_ASSIGN_OR_RETURN(Expr * lhs, sub_production());
  while (true) {
    XLS_VLOG(5) << "Binop chain lhs: " << lhs->ToString();
    bool peek_in_targets;
    if (absl::holds_alternative<absl::Span<TokenKind const>>(target_tokens)) {
      XLS_ASSIGN_OR_RETURN(
          peek_in_targets,
          PeekTokenIn(absl::get<absl::Span<TokenKind const>>(target_tokens)));
    } else {
      XLS_ASSIGN_OR_RETURN(
          peek_in_targets,
          PeekKeywordIn(absl::get<absl::Span<Keyword const>>(target_tokens)));
    }
    if (peek_in_targets) {
      Token op = PopTokenOrDie();
      XLS_ASSIGN_OR_RETURN(Expr * rhs, sub_production());
      XLS_ASSIGN_OR_RETURN(BinopKind kind,
                           BinopKindFromString(TokenKindToString(op.kind())));
      lhs = module_->Make<Binop>(op.span(), kind, lhs, rhs);
    } else {
      break;
    }
  }
  XLS_VLOG(5) << "Binop chain result: " << lhs->ToString();
  return lhs;
}

absl::StatusOr<Expr*> Parser::ParseComparisonExpression(Bindings* bindings) {
  XLS_VLOG(5) << "ParseComparisonExpression; start";
  XLS_ASSIGN_OR_RETURN(Expr * lhs, ParseOrExpression(bindings));
  while (true) {
    XLS_VLOG(5) << "ParseComparisonExpression; lhs: " << lhs->ToString()
                << " peek: " << PeekToken().value()->ToString();
    XLS_ASSIGN_OR_RETURN(bool peek_in_targets, PeekTokenIn(kComparisonKinds));
    if (!peek_in_targets) {
      XLS_VLOG(5) << "Peek is not in comparison kinds.";
      break;
    }

    Transaction txn(this, bindings);
    auto cleanup = absl::MakeCleanup([&txn]() { txn.Rollback(); });
    Token op = PopTokenOrDie();
    auto status_or_rhs = ParseOrExpression(txn.bindings());
    XLS_VLOG(5) << "rhs status: " << status_or_rhs.status();
    if (status_or_rhs.ok()) {
      XLS_ASSIGN_OR_RETURN(BinopKind kind,
                           BinopKindFromString(TokenKindToString(op.kind())));
      lhs = module_->Make<Binop>(op.span(), kind, lhs, status_or_rhs.value());
      txn.CommitAndCancelCleanup(&cleanup);
    } else {
      break;
    }
  }
  XLS_VLOG(5) << "ParseComparisonExpression; result: " << lhs->ToString();
  return lhs;
}

absl::StatusOr<NameDefTree*> Parser::ParsePattern(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(absl::optional<Token> oparen,
                       TryPopToken(TokenKind::kOParen));
  if (oparen) {
    return ParseTuplePattern(oparen->span().start(), bindings);
  }

  XLS_ASSIGN_OR_RETURN(const Token* peek, PeekToken());
  if (peek->kind() == TokenKind::kIdentifier) {
    XLS_ASSIGN_OR_RETURN(Token tok, PopTokenOrError(TokenKind::kIdentifier));
    if (*tok.GetValue() == "_") {
      return module_->Make<NameDefTree>(
          tok.span(), module_->Make<WildcardPattern>(tok.span()));
    }
    XLS_ASSIGN_OR_RETURN(bool peek_is_double_colon,
                         PeekTokenIs(TokenKind::kDoubleColon));
    if (peek_is_double_colon) {  // Mod or enum ref.
      XLS_ASSIGN_OR_RETURN(NameRef * subject, ParseNameRef(bindings, &tok));
      XLS_ASSIGN_OR_RETURN(ColonRef * colon_ref,
                           ParseColonRef(bindings, subject));
      return module_->Make<NameDefTree>(tok.span(), colon_ref);
    }

    absl::optional<BoundNode> resolved = bindings->ResolveNode(*tok.GetValue());
    NameRef* ref;
    if (resolved) {
      AnyNameDef name_def =
          bindings->ResolveNameOrNullopt(*tok.GetValue()).value();
      if (absl::holds_alternative<ConstantDef*>(*resolved)) {
        ref = module_->Make<ConstRef>(tok.span(), *tok.GetValue(), name_def);
      } else {
        ref = module_->Make<NameRef>(tok.span(), *tok.GetValue(), name_def);
      }
      return module_->Make<NameDefTree>(tok.span(), ref);
    }

    // If the name is not bound, this pattern is creating a binding.
    XLS_ASSIGN_OR_RETURN(NameDef * name_def, TokenToNameDef(tok));
    bindings->Add(name_def->identifier(), name_def);
    auto* result = module_->Make<NameDefTree>(tok.span(), name_def);
    name_def->set_definer(result);
    return result;
  }

  if (peek->IsKindIn({TokenKind::kNumber, TokenKind::kCharacter, Keyword::kTrue,
                      Keyword::kFalse}) ||
      peek->IsTypeKeyword()) {
    XLS_ASSIGN_OR_RETURN(Number * number, ParseNumber(bindings));
    return module_->Make<NameDefTree>(number->span(), number);
  }

  return ParseErrorStatus(
      peek->span(),
      absl::StrFormat("Expected pattern; got %s", peek->ToErrorString()));
}

absl::StatusOr<Match*> Parser::ParseMatch(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(Token match, PopKeywordOrError(Keyword::kMatch));
  XLS_ASSIGN_OR_RETURN(Expr * matched, ParseExpression(bindings));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOBrace));

  std::vector<MatchArm*> arms;
  bool must_end = false;
  while (true) {
    XLS_ASSIGN_OR_RETURN(bool dropped_cbrace, TryDropToken(TokenKind::kCBrace));
    if (dropped_cbrace) {
      break;
    }
    if (must_end) {
      XLS_RETURN_IF_ERROR(
          DropTokenOrError(TokenKind::kCBrace, nullptr,
                           "Expected '}' because no ',' was seen to indicate "
                           "an additional match case."));
      break;
    }
    Bindings arm_bindings(bindings);
    XLS_ASSIGN_OR_RETURN(NameDefTree * first_pattern,
                         ParsePattern(&arm_bindings));
    std::vector<NameDefTree*> patterns = {first_pattern};
    while (true) {
      XLS_ASSIGN_OR_RETURN(bool dropped_bar, TryDropToken(TokenKind::kBar));
      if (!dropped_bar) {
        break;
      }
      if (arm_bindings.HasLocalBindings()) {
        // TODO(leary): 2020-09-12 Loosen this restriction? They just have to
        // bind the same exact set of names.
        return ParseErrorStatus(
            first_pattern->span(),
            "Cannot have multiple patterns that bind names.");
      }
      XLS_ASSIGN_OR_RETURN(NameDefTree * pattern, ParsePattern(&arm_bindings));
      patterns.push_back(pattern);
    }
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kFatArrow));
    XLS_ASSIGN_OR_RETURN(Expr * rhs, ParseExpression(&arm_bindings));
    Span span(patterns[0]->span().start(), rhs->span().limit());
    arms.push_back(module_->Make<MatchArm>(span, std::move(patterns), rhs));
    XLS_ASSIGN_OR_RETURN(bool dropped_comma, TryDropToken(TokenKind::kComma));
    must_end = !dropped_comma;
  }
  Span span(match.span().start(), GetPos());
  return module_->Make<Match>(span, matched, std::move(arms));
}

absl::StatusOr<Import*> Parser::ParseImport(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(Token kw, PopKeywordOrError(Keyword::kImport));
  XLS_ASSIGN_OR_RETURN(Token tok, PopTokenOrError(TokenKind::kIdentifier));
  std::vector<Token> toks = {tok};
  std::vector<std::string> subject = {*tok.GetValue()};
  while (true) {
    XLS_ASSIGN_OR_RETURN(bool dropped_dot, TryDropToken(TokenKind::kDot));
    if (!dropped_dot) {
      break;
    }
    XLS_ASSIGN_OR_RETURN(Token tok, PopTokenOrError(TokenKind::kIdentifier));
    toks.push_back(tok);
    subject.push_back(*tok.GetValue());
  }

  XLS_ASSIGN_OR_RETURN(bool dropped_as, TryDropKeyword(Keyword::kAs));
  NameDef* name_def;
  absl::optional<std::string> alias;
  if (dropped_as) {
    XLS_ASSIGN_OR_RETURN(name_def, ParseNameDef(bindings));
    alias = name_def->identifier();
  } else {
    XLS_ASSIGN_OR_RETURN(name_def, TokenToNameDef(toks.back()));
  }
  auto* import = module_->Make<Import>(kw.span(), subject, name_def, alias);
  name_def->set_definer(import);
  bindings->Add(name_def->identifier(), import);
  return import;
}

absl::StatusOr<Function*> Parser::ParseFunctionInternal(
    bool is_public, Bindings* outer_bindings) {
  XLS_ASSIGN_OR_RETURN(Token fn_tok, PopKeywordOrError(Keyword::kFn));
  const Pos start_pos = fn_tok.span().start();

  XLS_ASSIGN_OR_RETURN(NameDef * name_def, ParseNameDef(outer_bindings));

  Bindings bindings(outer_bindings);
  bindings.Add(name_def->identifier(), name_def);

  XLS_ASSIGN_OR_RETURN(bool dropped_oangle, TryDropToken(TokenKind::kOAngle));
  std::vector<ParametricBinding*> parametric_bindings;
  if (dropped_oangle) {  // Parametric.
    XLS_ASSIGN_OR_RETURN(parametric_bindings,
                         ParseParametricBindings(&bindings));
  }

  XLS_ASSIGN_OR_RETURN(std::vector<Param*> params, ParseParams(&bindings));

  XLS_ASSIGN_OR_RETURN(bool dropped_arrow, TryDropToken(TokenKind::kArrow));
  TypeAnnotation* return_type = nullptr;
  if (dropped_arrow) {
    XLS_ASSIGN_OR_RETURN(return_type, ParseTypeAnnotation(&bindings));
  }

  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOBrace));
  XLS_ASSIGN_OR_RETURN(Expr * body, ParseExpression(&bindings));
  XLS_ASSIGN_OR_RETURN(
      Token end_brace,
      PopTokenOrError(TokenKind::kCBrace, nullptr,
                      "Expected '}' at end of function body."));
  Function* f = module_->Make<Function>(
      Span(start_pos, end_brace.span().limit()), name_def, parametric_bindings,
      params, return_type, body, Function::Tag::kNormal, is_public);
  name_def->set_definer(f);
  return f;
}

absl::StatusOr<QuickCheck*> Parser::ParseQuickCheck(
    absl::flat_hash_map<std::string, Function*>* name_to_fn, Bindings* bindings,
    const Span& directive_span) {
  absl::optional<int64_t> test_count;
  XLS_ASSIGN_OR_RETURN(bool peek_is_paren, PeekTokenIs(TokenKind::kOParen));
  if (peek_is_paren) {  // Config is specified.
    DropTokenOrDie();
    XLS_ASSIGN_OR_RETURN(std::string config_name, PopIdentifierOrError());
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kEquals));
    if (config_name == "test_count") {
      XLS_ASSIGN_OR_RETURN(Token count_token,
                           PopTokenOrError(TokenKind::kNumber));
      XLS_ASSIGN_OR_RETURN(test_count, count_token.GetValueAsInt64());
      if (test_count <= 0) {
        return ParseErrorStatus(
            count_token.span(),
            absl::StrFormat("Number of tests should be > 0, got %d",
                            *test_count));
      }
      XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCParen));
    } else {
      return ParseErrorStatus(
          directive_span,
          absl::StrFormat("Unknown configuration key in directive: '%s'",
                          config_name));
    }
  }

  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCBrack));
  XLS_ASSIGN_OR_RETURN(
      Function * fn, ParseFunction(/*is_public=*/false, bindings, name_to_fn));
  return module_->Make<QuickCheck>(fn->span(), fn, test_count);
}

absl::StatusOr<XlsTuple*> Parser::ParseTupleRemainder(const Pos& start_pos,
                                                      Expr* first,
                                                      Bindings* bindings) {
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kComma));
  auto parse_expression = [this, bindings]() -> absl::StatusOr<Expr*> {
    return ParseExpression(bindings);
  };
  XLS_ASSIGN_OR_RETURN(
      std::vector<Expr*> es,
      ParseCommaSeq<Expr*>(parse_expression, TokenKind::kCParen));
  es.insert(es.begin(), first);
  Span span(start_pos, GetPos());
  return module_->Make<XlsTuple>(span, std::move(es));
}

absl::StatusOr<Expr*> Parser::ParseTerm(Bindings* outer_bindings) {
  XLS_ASSIGN_OR_RETURN(const Token* peek, PeekToken());
  const Pos start_pos = peek->span().start();

  bool peek_is_kw_in = peek->IsKeyword(Keyword::kIn);
  bool peek_is_kw_out = peek->IsKeyword(Keyword::kOut);

  Expr* lhs = nullptr;
  if (peek->IsKindIn({TokenKind::kNumber, TokenKind::kCharacter}) ||
      peek->IsKeywordIn({Keyword::kTrue, Keyword::kFalse})) {
    XLS_ASSIGN_OR_RETURN(lhs, ParseNumber(outer_bindings));
  } else if (peek->IsKindIn({TokenKind::kDoubleQuote})) {
    // Eat characters until the first unescaped double quote.
    Span span = peek->span();
    XLS_ASSIGN_OR_RETURN(std::string text, PopString());
    if (text.empty()) {
      // TODO(rspringer): 2021-05-20 Add zero-length support.
      return ParseErrorStatus(peek->span(),
                              "Zero-length strings are not supported.");
    }
    return module_->Make<String>(Span(start_pos, GetPos()), text);
  } else if (peek->IsKindIn({TokenKind::kBang, TokenKind::kMinus})) {
    Token tok = PopTokenOrDie();
    XLS_ASSIGN_OR_RETURN(Expr * arg, ParseTerm(outer_bindings));
    UnopKind unop_kind;
    switch (tok.kind()) {
      // clang-format off
      case TokenKind::kBang: unop_kind = UnopKind::kInvert; break;
      case TokenKind::kMinus: unop_kind = UnopKind::kNegate; break;
      // clang-format on
      default:
        XLS_LOG(FATAL) << "Inconsistent unary operation token kind.";
    }
    Span span(start_pos, GetPos());
    lhs = module_->Make<Unop>(span, unop_kind, arg);
  } else if (peek->IsTypeKeyword() ||
             (peek->kind() == TokenKind::kIdentifier &&
              outer_bindings->ResolveNodeIsTypeDefinition(*peek->GetValue()))) {
    XLS_ASSIGN_OR_RETURN(lhs,
                         ParseCastOrEnumRefOrStructInstance(outer_bindings));
  } else if (peek->IsKeyword(Keyword::kRecv)) {
    Token recv = PopTokenOrDie();
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOParen));
    XLS_ASSIGN_OR_RETURN(NameRef * token, ParseNameRef(outer_bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kComma));
    XLS_ASSIGN_OR_RETURN(NameRef * channel, ParseNameRef(outer_bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCParen));
    return module_->Make<Recv>(Span(recv.span().start(), GetPos()), token,
                               channel);
  } else if (peek->IsKeyword(Keyword::kRecvIf)) {
    Token recv = PopTokenOrDie();
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOParen));
    XLS_ASSIGN_OR_RETURN(NameRef * token, ParseNameRef(outer_bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kComma));
    XLS_ASSIGN_OR_RETURN(NameRef * channel, ParseNameRef(outer_bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kComma));
    XLS_ASSIGN_OR_RETURN(Expr * condition, ParseExpression(outer_bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCParen));
    return module_->Make<RecvIf>(Span(recv.span().start(), GetPos()), token,
                                 channel, condition);
  } else if (peek->IsKeyword(Keyword::kSend)) {
    Token send = PopTokenOrDie();
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOParen));
    XLS_ASSIGN_OR_RETURN(NameRef * token, ParseNameRef(outer_bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kComma));
    XLS_ASSIGN_OR_RETURN(NameRef * channel, ParseNameRef(outer_bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kComma));
    XLS_ASSIGN_OR_RETURN(Expr * payload, ParseExpression(outer_bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCParen));
    Pos end = GetPos();
    return module_->Make<Send>(Span(send.span().start(), end), token, channel,
                               payload);
  } else if (peek->IsKeyword(Keyword::kSendIf)) {
    Token send = PopTokenOrDie();
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOParen));
    XLS_ASSIGN_OR_RETURN(NameRef * token, ParseNameRef(outer_bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kComma));
    XLS_ASSIGN_OR_RETURN(NameRef * channel, ParseNameRef(outer_bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kComma));
    XLS_ASSIGN_OR_RETURN(Expr * condition, ParseExpression(outer_bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kComma));
    XLS_ASSIGN_OR_RETURN(Expr * payload, ParseExpression(outer_bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCParen));
    Pos end = GetPos();
    return module_->Make<SendIf>(Span(send.span().start(), end), token, channel,
                                 condition, payload);
  } else if (peek->IsKeyword(Keyword::kJoin)) {
    Token join = PopTokenOrDie();
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOParen));
    XLS_ASSIGN_OR_RETURN(
        std::vector<Expr*> tokens,
        ParseCommaSeq(BindFront(&Parser::ParseExpression, outer_bindings),
                      TokenKind::kCParen));
    return module_->Make<Join>(Span(join.span().start(), GetPos()), tokens);
  } else if (peek->kind() == TokenKind::kIdentifier || peek_is_kw_in ||
             peek_is_kw_out) {
    std::string lhs_str = *peek->GetValue();
    if (peek_is_kw_in) {
      lhs_str = "in";
    } else if (peek_is_kw_out) {
      lhs_str = "out";
    }
    XLS_ASSIGN_OR_RETURN(auto nocr, ParseNameOrColonRef(outer_bindings));
    if (absl::holds_alternative<ColonRef*>(nocr)) {
      XLS_ASSIGN_OR_RETURN(bool peek_is_obrace,
                           PeekTokenIs(TokenKind::kOBrace));
      if (peek_is_obrace) {
        ColonRef* colon_ref = absl::get<ColonRef*>(nocr);
        TypeRef* type_ref =
            module_->Make<TypeRef>(colon_ref->span(), lhs_str, colon_ref);
        XLS_ASSIGN_OR_RETURN(
            TypeAnnotation * type,
            MakeTypeRefTypeAnnotation(colon_ref->span(), type_ref, {}, {}));
        Transaction inner_txn(this, outer_bindings);
        auto cleanup =
            absl::MakeCleanup([&inner_txn]() { inner_txn.Rollback(); });
        // We see a brace after our colon-ref, and that could be a struct
        // identifier to instantiate -- see if we can parse a struct instance
        // here. If not, we fall back to just the colon-ref.
        auto statusor = ParseStructInstance(outer_bindings, type);
        if (statusor.ok()) {
          inner_txn.CommitAndCancelCleanup(&cleanup);
          return statusor.value();
        }
        return colon_ref;
      }
    }
    lhs = ToExprNode(nocr);
  } else if (peek->kind() == TokenKind::kOParen) {  // Parenthesized expression.
    // An empty set of parenthesed could be either an empty tuple or an empty
    // tuple _type_annotation_. We disambiguate the two by discounting the
    // latter result if not followed by a colon.
    {
      Transaction inner_txn(this, outer_bindings);
      auto cleanup =
          absl::MakeCleanup([&inner_txn]() { inner_txn.Rollback(); });
      auto status_or_annot = ParseTypeAnnotation(inner_txn.bindings());
      if (status_or_annot.ok()) {
        if (DropTokenOrError(TokenKind::kColon).ok()) {
          inner_txn.CommitAndCancelCleanup(&cleanup);
        }
        // If there was no colon, then we'll try another production.
      }
    }

    Token oparen = PopTokenOrDie();
    XLS_ASSIGN_OR_RETURN(bool next_is_cparen, PeekTokenIs(TokenKind::kCParen));
    if (next_is_cparen) {  // Empty tuple.
      XLS_ASSIGN_OR_RETURN(Token tok, PopToken());
      Span span(start_pos, GetPos());
      lhs = module_->Make<XlsTuple>(span, std::vector<Expr*>{});
    } else {
      XLS_ASSIGN_OR_RETURN(lhs, ParseExpression(outer_bindings));
      XLS_ASSIGN_OR_RETURN(bool peek_is_comma, PeekTokenIs(TokenKind::kComma));
      if (peek_is_comma) {  // Singleton tuple.
        XLS_ASSIGN_OR_RETURN(lhs, ParseTupleRemainder(oparen.span().start(),
                                                      lhs, outer_bindings));
      } else {
        XLS_RETURN_IF_ERROR(
            DropTokenOrError(TokenKind::kCParen, /*start=*/&oparen));
      }
    }
  } else if (peek->IsKeyword(Keyword::kMatch)) {  // Match expression.
    XLS_ASSIGN_OR_RETURN(lhs, ParseMatch(outer_bindings));
  } else if (peek->kind() == TokenKind::kOBrack) {  // Array expression.
    XLS_ASSIGN_OR_RETURN(lhs, ParseArray(outer_bindings));
  } else if (peek->IsKeyword(Keyword::kIf)) {  // Ternary expression.
    XLS_ASSIGN_OR_RETURN(lhs, ParseTernaryExpression(outer_bindings));
  } else {
    return ParseErrorStatus(
        peek->span(),
        absl::StrFormat("Expected start of an expression; got: %s",
                        peek->ToErrorString()));
  }
  XLS_CHECK(lhs != nullptr);

  while (true) {
    const Pos new_pos = GetPos();
    XLS_ASSIGN_OR_RETURN(const Token* peek, PeekToken());
    switch (peek->kind()) {
      case TokenKind::kColon: {  // Possibly a Number of ColonRef type.
        Span span(new_pos, GetPos());
        // The only valid construct here would be declaring a number via
        // ColonRef-colon-Number, e.g., "module::type:7"
        if (dynamic_cast<ColonRef*>(lhs) == nullptr) {
          goto done;
        }
        auto* type_ref = module_->Make<TypeRef>(span, lhs->ToString(),
                                                ToTypeDefinition(lhs).value());
        auto* type_annot = module_->Make<TypeRefTypeAnnotation>(
            span, type_ref, std::vector<Expr*>());
        XLS_ASSIGN_OR_RETURN(lhs, ParseCast(outer_bindings, type_annot));
        break;
      }
      case TokenKind::kOParen: {  // Invocation.
        DropTokenOrDie();
        XLS_ASSIGN_OR_RETURN(
            std::vector<Expr*> args,
            ParseCommaSeq(BindFront(&Parser::ParseExpression, outer_bindings),
                          TokenKind::kCParen));
        XLS_ASSIGN_OR_RETURN(
            lhs, BuildMacroOrInvocation(Span(new_pos, GetPos()), lhs,
                                        std::move(args)));
        break;
      }
      case TokenKind::kDot: {
        DropTokenOrDie();
        XLS_ASSIGN_OR_RETURN(Token tok,
                             PopTokenOrError(TokenKind::kIdentifier));
        XLS_ASSIGN_OR_RETURN(NameDef * attr, TokenToNameDef(tok));
        Span span(new_pos, GetPos());
        lhs = module_->Make<Attr>(span, lhs, attr);
        break;
      }
      case TokenKind::kOBrack: {
        DropTokenOrDie();
        XLS_ASSIGN_OR_RETURN(bool dropped_colon,
                             TryDropToken(TokenKind::kColon));
        if (dropped_colon) {  // Slice-from-beginning.
          XLS_ASSIGN_OR_RETURN(lhs, ParseBitSlice(new_pos, lhs, outer_bindings,
                                                  /*start=*/nullptr));
          break;
        }
        XLS_ASSIGN_OR_RETURN(Expr * index, ParseExpression(outer_bindings));
        XLS_ASSIGN_OR_RETURN(peek, PeekToken());
        switch (peek->kind()) {
          case TokenKind::kPlusColon: {  // Explicit width slice.
            DropTokenOrDie();
            Expr* start = index;
            XLS_ASSIGN_OR_RETURN(TypeAnnotation * width,
                                 ParseTypeAnnotation(outer_bindings));
            Span span(new_pos, GetPos());
            auto* width_slice = module_->Make<WidthSlice>(span, start, width);
            lhs = module_->Make<Index>(span, lhs, width_slice);
            XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCBrack));
            break;
          }
          case TokenKind::kColon: {  // Slice to end.
            DropTokenOrDie();
            XLS_ASSIGN_OR_RETURN(
                lhs, ParseBitSlice(new_pos, lhs, outer_bindings, index));
            break;
          }
          default:
            XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCBrack));
            lhs = module_->Make<Index>(Span(new_pos, GetPos()), lhs, index);
        }
        break;
      }
      case TokenKind::kOAngle: {
        // Comparison op or parametric function invocation.
        Transaction sub_txn(this, outer_bindings);
        auto sub_cleanup =
            absl::MakeCleanup([&sub_txn]() { sub_txn.Rollback(); });

        auto status_or_parametrics = ParseParametrics(sub_txn.bindings());
        if (!status_or_parametrics.ok()) {
          goto done;
        }

        XLS_ASSIGN_OR_RETURN(Token tok, PopTokenOrError(TokenKind::kOParen));
        XLS_ASSIGN_OR_RETURN(std::vector<Expr*> args,
                             ParseCommaSeq(BindFront(&Parser::ParseExpression,
                                                     sub_txn.bindings()),
                                           TokenKind::kCParen));
        XLS_ASSIGN_OR_RETURN(
            lhs, BuildMacroOrInvocation(Span(new_pos, GetPos()), lhs,
                                        std::move(args),
                                        status_or_parametrics.value()));
        sub_txn.CommitAndCancelCleanup(&sub_cleanup);
        break;
      }
      case TokenKind::kArrow:
        // If we're a term followed by an arrow...then we followed the wrong
        // production, as arrows are only allowed after fn decls. Rewind.
        // Should this be something else, like a "wrong production" error?
        return ParseErrorStatus(
            lhs->span(), "Parenthesized expression cannot precede an arrow.");
      default:
        goto done;
    }
  }

done:
  return lhs;
}

absl::StatusOr<Expr*> Parser::BuildMacroOrInvocation(
    Span span, Expr* callee, std::vector<Expr*> args,
    std::vector<Expr*> parametrics) {
  if (auto* name_ref = dynamic_cast<NameRef*>(callee)) {
    if (auto* builtin = TryGet<BuiltinNameDef*>(name_ref->name_def())) {
      std::string name = builtin->identifier();
      if (name == "trace_fmt!") {
        if (!parametrics.empty()) {
          return ParseErrorStatus(
              span, absl::StrFormat(
                        "%s macro does not take parametric arguments", name));
        }
        if (args.empty()) {
          return ParseErrorStatus(
              span, absl::StrFormat("%s macro must have at least one argument",
                                    name));
        }

        Expr* format_arg = args[0];
        if (String* format_string = dynamic_cast<String*>(format_arg)) {
          const std::string& format_text = format_string->text();
          absl::StatusOr<std::vector<FormatStep>> format_result =
              ParseFormatString(format_text);
          if (!format_result.ok()) {
            return ParseErrorStatus(format_string->span(),
                                    format_result.status().message());
          }
          // Remove the format string argument before building the macro call.
          args.erase(args.begin());
          return module_->Make<FormatMacro>(span, name, format_result.value(),
                                            args);
        }

        return ParseErrorStatus(
            span, absl::StrFormat("The first argument of the %s macro must "
                                  "be a literal string.",
                                  name));
      }
    }
  }
  return module_->Make<Invocation>(span, callee, std::move(args),
                                   std::move(parametrics));
}

absl::StatusOr<Spawn*> Parser::ParseSpawn(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(Token spawn, PopKeywordOrError(Keyword::kSpawn));
  XLS_ASSIGN_OR_RETURN(auto name_or_colon_ref, ParseNameOrColonRef(bindings));

  std::vector<Expr*> parametrics;
  XLS_ASSIGN_OR_RETURN(bool peek_is_oangle, PeekTokenIs(TokenKind::kOAngle));
  if (peek_is_oangle) {
    XLS_ASSIGN_OR_RETURN(parametrics, ParseParametrics(bindings));
  }

  Expr* spawnee;
  Expr* config_ref;
  Expr* next_ref;
  if (absl::holds_alternative<NameRef*>(name_or_colon_ref)) {
    NameRef* name_ref = absl::get<NameRef*>(name_or_colon_ref);
    spawnee = name_ref;
    // We avoid name collisions b/w exiesting functions and Proc config/next fns
    // by using a "." as the separator, which is invalid for function
    // specifications.
    std::string config_name = absl::StrCat(name_ref->identifier(), ".config");
    std::string next_name = absl::StrCat(name_ref->identifier(), ".next");
    XLS_ASSIGN_OR_RETURN(
        AnyNameDef config_def,
        bindings->ResolveNameOrError(config_name, spawnee->span()));
    if (!absl::holds_alternative<NameDef*>(config_def)) {
      return absl::InternalError("Proc config should be named \".config\"");
    }
    config_ref =
        module_->Make<NameRef>(name_ref->span(), config_name, config_def);

    XLS_ASSIGN_OR_RETURN(AnyNameDef next_def, bindings->ResolveNameOrError(
                                                  next_name, spawnee->span()));
    if (!absl::holds_alternative<NameDef*>(next_def)) {
      return absl::InternalError("Proc next should be named \".next\"");
    }
    next_ref = module_->Make<NameRef>(name_ref->span(), next_name, next_def);
  } else {
    ColonRef* colon_ref = absl::get<ColonRef*>(name_or_colon_ref);
    spawnee = colon_ref;

    config_ref =
        module_->Make<ColonRef>(colon_ref->span(), colon_ref->subject(),
                                absl::StrCat(colon_ref->attr(), ".config"));
    next_ref =
        module_->Make<ColonRef>(colon_ref->span(), colon_ref->subject(),
                                absl::StrCat(colon_ref->attr(), ".next"));
  }

  auto parse_args = [this, bindings] { return ParseExpression(bindings); };
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOParen));
  Pos config_start = GetPos();
  XLS_ASSIGN_OR_RETURN(std::vector<Expr*> config_args,
                       ParseCommaSeq<Expr*>(parse_args, TokenKind::kCParen));
  Pos config_limit = GetPos();

  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOParen));
  Pos next_start = GetPos();
  XLS_ASSIGN_OR_RETURN(std::vector<Expr*> next_args,
                       ParseCommaSeq<Expr*>(parse_args, TokenKind::kCParen));
  Pos next_limit = GetPos();

  // Spawn can be the last item in a proc.
  Expr* body = nullptr;
  XLS_ASSIGN_OR_RETURN(bool peek_is_semi, PeekTokenIs(TokenKind::kSemi));
  if (peek_is_semi) {
    DropTokenOrDie();
    XLS_ASSIGN_OR_RETURN(body, ParseExpression(bindings));
  }

  auto* config_invoc = module_->Make<Invocation>(
      Span(config_start, config_limit), config_ref, config_args, parametrics);

  auto* next_invoc = module_->Make<Invocation>(
      Span(next_start, next_limit), next_ref, next_args, parametrics);

  return module_->Make<Spawn>(Span(spawn.span().start(), next_limit), spawnee,
                              config_invoc, next_invoc, parametrics, body);
}

absl::StatusOr<Index*> Parser::ParseBitSlice(const Pos& start_pos, Expr* lhs,
                                             Bindings* bindings, Expr* start) {
  Expr* limit_expr = nullptr;
  XLS_ASSIGN_OR_RETURN(bool peek_is_cbrack, PeekTokenIs(TokenKind::kCBrack));
  if (!peek_is_cbrack) {
    XLS_ASSIGN_OR_RETURN(limit_expr, ParseExpression(bindings));
  }

  XLS_RETURN_IF_ERROR(
      DropTokenOrError(TokenKind::kCBrack, nullptr, "at end of bit slice"));

  // Type deduction will verify that start & limit are constexpr.
  Slice* index =
      module_->Make<Slice>(Span(start_pos, GetPos()), start, limit_expr);
  return module_->Make<Index>(Span(start_pos, GetPos()), lhs, index);
}

absl::StatusOr<Expr*> Parser::ParseCastAsExpression(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(Expr * lhs, ParseTerm(bindings));
  while (true) {
    XLS_ASSIGN_OR_RETURN(bool dropped_as, TryDropKeyword(Keyword::kAs));
    if (!dropped_as) {
      break;
    }
    XLS_ASSIGN_OR_RETURN(TypeAnnotation * type, ParseTypeAnnotation(bindings));
    Span span(lhs->span().start(), type->span().limit());
    lhs = module_->Make<Cast>(span, lhs, type);
  }
  return lhs;
}

absl::StatusOr<ConstantDef*> Parser::ParseConstantDef(bool is_public,
                                                      Bindings* bindings) {
  const Pos start_pos = GetPos();
  XLS_RETURN_IF_ERROR(DropKeywordOrError(Keyword::kConst));
  Bindings new_bindings(/*parent=*/bindings);
  XLS_ASSIGN_OR_RETURN(NameDef * name_def, ParseNameDef(&new_bindings));
  if (bindings->HasName(name_def->identifier())) {
    Span span =
        BoundNodeGetSpan(bindings->ResolveNode(name_def->identifier()).value());
    return ParseErrorStatus(
        name_def->span(),
        absl::StrFormat(
            "Constant definition is shadowing an existing definition from %s",
            span.ToString()));
  }

  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kEquals));
  XLS_ASSIGN_OR_RETURN(Expr * expr, ParseExpression(bindings));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kSemi));
  Span span(start_pos, GetPos());
  auto* result = module_->Make<ConstantDef>(span, name_def, expr, is_public,
                                            /*is_local=*/false);
  name_def->set_definer(result);
  bindings->Add(name_def->identifier(), result);
  return result;
}

absl::StatusOr<std::vector<Param*>> Parser::CollectProcMembers(
    Bindings* bindings) {
  std::vector<Param*> members;
  Transaction txn(this, bindings);
  auto cleanup = absl::MakeCleanup([&txn] { txn.Rollback(); });

  XLS_ASSIGN_OR_RETURN(const Token* peek, PeekToken());
  while (!peek->IsKeyword(Keyword::kConfig)) {
    XLS_ASSIGN_OR_RETURN(Param * param, ParseParam(bindings));
    members.push_back(param);
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kSemi));
    XLS_ASSIGN_OR_RETURN(peek, PeekToken());
  }

  for (const auto* member : members) {
    bindings->Add(member->identifier(), member->name_def());
  }

  txn.CommitAndCancelCleanup(&cleanup);
  return members;
}

absl::StatusOr<Function*> Parser::ParseProcConfig(
    Bindings* outer_bindings,
    const std::vector<ParametricBinding*>& parametric_bindings,
    const std::vector<Param*>& proc_members, absl::string_view proc_name) {
  Bindings bindings(outer_bindings);
  XLS_ASSIGN_OR_RETURN(const Token* peek, PeekToken());
  if (!peek->IsKeyword(Keyword::kConfig)) {
    return ParseErrorStatus(
        peek->span(),
        absl::StrCat("Expected 'config', got ", peek->GetStringValue()));
  }

  XLS_RETURN_IF_ERROR(DropToken());
  XLS_ASSIGN_OR_RETURN(Token oparen, PopTokenOrError(TokenKind::kOParen));

  auto parse_param = [this, &bindings]() -> absl::StatusOr<Param*> {
    return ParseParam(&bindings);
  };
  XLS_ASSIGN_OR_RETURN(std::vector<Param*> config_params,
                       ParseCommaSeq<Param*>(parse_param, TokenKind::kCParen));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOBrace));
  XLS_ASSIGN_OR_RETURN(Expr * body, ParseExpression(&bindings));

  // TODO(rspringer): 2021-10-13: Rework this when issue #507 is
  // resolved - when let expressions can be processed sequentially instead
  // of recursively.
  Expr* final_expr = body;
  auto as_let = dynamic_cast<Let*>(final_expr);
  auto as_spawn = dynamic_cast<Spawn*>(final_expr);
  while (as_let != nullptr || as_spawn != nullptr) {
    if (as_let != nullptr) {
      final_expr = as_let->body();
      as_spawn = dynamic_cast<Spawn*>(as_let->body());
      as_let = dynamic_cast<Let*>(as_let->body());
    } else {
      final_expr = as_spawn->body();
      as_let = dynamic_cast<Let*>(as_spawn->body());
      as_spawn = dynamic_cast<Spawn*>(as_spawn->body());
    }
  }

  if (dynamic_cast<XlsTuple*>(final_expr) == nullptr) {
    return ParseErrorStatus(
        body->span(),
        "The final expression in a Proc config must be a tuple with one "
        "element for each Proc data member.");
  }
  XLS_ASSIGN_OR_RETURN(Token cbrace, PopTokenOrError(TokenKind::kCBrace));

  Span span(oparen.span().start(), cbrace.span().limit());
  NameDef* name_def =
      module_->Make<NameDef>(span, absl::StrCat(proc_name, ".config"), nullptr);
  std::vector<TypeAnnotation*> return_elements;
  return_elements.reserve(proc_members.size());
  for (const auto* member : proc_members) {
    return_elements.push_back(member->type_annotation());
  }
  TypeAnnotation* return_type =
      module_->Make<TupleTypeAnnotation>(span, return_elements);
  Function* config = module_->Make<Function>(
      span, name_def, parametric_bindings, config_params, return_type, body,
      Function::Tag::kProcConfig, /*is_public=*/false);
  name_def->set_definer(config);

  return config;
}

bool TypeIsToken(TypeAnnotation* type) {
  auto* builtin_type = dynamic_cast<BuiltinTypeAnnotation*>(type);
  if (builtin_type == nullptr) {
    return false;
  }

  return builtin_type->builtin_type() == BuiltinType::kToken;
}

absl::StatusOr<Function*> Parser::ParseProcNext(
    Bindings* outer_bindings,
    const std::vector<ParametricBinding*>& parametric_bindings,
    absl::string_view proc_name) {
  Bindings bindings(outer_bindings);
  XLS_ASSIGN_OR_RETURN(const Token* peek, PeekToken());
  if (!peek->IsKeyword(Keyword::kNext)) {
    return ParseErrorStatus(peek->span(), absl::StrCat("Expected 'next', got ",
                                                       peek->GetStringValue()));
  }
  XLS_RETURN_IF_ERROR(DropToken());
  XLS_ASSIGN_OR_RETURN(Token oparen, PopTokenOrError(TokenKind::kOParen));

  auto parse_param = [this, &bindings]() -> absl::StatusOr<Param*> {
    return ParseParam(&bindings);
  };
  XLS_ASSIGN_OR_RETURN(std::vector<Param*> next_params,
                       ParseCommaSeq<Param*>(parse_param, TokenKind::kCParen));
  std::vector<TypeAnnotation*> return_elements;
  if (next_params.empty() || !TypeIsToken(next_params[0]->type_annotation())) {
    return ParseErrorStatus(
        Span(GetPos(), GetPos()),
        "The first parameter in a Proc next function must be a token.");
  }

  for (int i = 1; i < next_params.size(); i++) {
    Param* param = next_params.at(i);
    if (dynamic_cast<ChannelTypeAnnotation*>(param->type_annotation()) !=
        nullptr) {
      return ParseErrorStatus(param->span(),
                              "Channels cannot be Proc next params.");
    }

    if (TypeIsToken(param->type_annotation())) {
      return ParseErrorStatus(
          param->span(),
          "Only the first parameter in a Proc next function may be a token.");
    }

    return_elements.push_back(param->type_annotation());
  }
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOBrace));
  XLS_ASSIGN_OR_RETURN(Expr * expr, ParseExpression(&bindings));
  XLS_ASSIGN_OR_RETURN(Token cbrace, PopTokenOrError(TokenKind::kCBrace));
  Span span(oparen.span().start(), cbrace.span().limit());
  TypeAnnotation* return_type =
      module_->Make<TupleTypeAnnotation>(span, return_elements);
  NameDef* name_def =
      module_->Make<NameDef>(span, absl::StrCat(proc_name, ".next"), nullptr);
  Function* next = module_->Make<Function>(
      Span(oparen.span().start(), cbrace.span().limit()), name_def,
      parametric_bindings, next_params, return_type, expr,
      Function::Tag::kProcNext,
      /*is_public=*/false);
  name_def->set_definer(next);

  return next;
}

absl::StatusOr<Proc*> Parser::ParseProc(bool is_public,
                                        Bindings* outer_bindings) {
  XLS_ASSIGN_OR_RETURN(Token proc_token, PopKeywordOrError(Keyword::kProc));
  XLS_ASSIGN_OR_RETURN(NameDef * name_def, ParseNameDef(outer_bindings));
  Bindings bindings(outer_bindings);
  bindings.Add(name_def->identifier(), name_def);

  XLS_ASSIGN_OR_RETURN(bool dropped_oangle, TryDropToken(TokenKind::kOAngle));
  std::vector<ParametricBinding*> parametric_bindings;
  if (dropped_oangle) {  // Parametric.
    XLS_ASSIGN_OR_RETURN(parametric_bindings,
                         ParseParametricBindings(&bindings));
  }

  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOBrace));
  std::vector<Param*> params;

  XLS_ASSIGN_OR_RETURN(std::vector<Param*> proc_members,
                       CollectProcMembers(&bindings));
  XLS_ASSIGN_OR_RETURN(Function * config,
                       ParseProcConfig(&bindings, parametric_bindings,
                                       proc_members, name_def->identifier()));
  module_->AddTop(config);
  outer_bindings->Add(config->name_def()->identifier(), config->name_def());

  XLS_ASSIGN_OR_RETURN(
      Function * next,
      ParseProcNext(&bindings, parametric_bindings, name_def->identifier()));
  module_->AddTop(next);
  outer_bindings->Add(next->name_def()->identifier(), next->name_def());

  XLS_ASSIGN_OR_RETURN(Token cbrace, PopTokenOrError(TokenKind::kCBrace));
  Span span(proc_token.span().start(), cbrace.span().limit());
  auto proc = module_->Make<Proc>(span, name_def, config->name_def(),
                                  next->name_def(), parametric_bindings,
                                  proc_members, config, next, is_public);
  name_def->set_definer(proc);
  config->set_proc(proc);
  next->set_proc(proc);
  return proc;
}

absl::StatusOr<ChannelDecl*> Parser::ParseChannelDecl(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(Token channel, PopKeywordOrError(Keyword::kChannel));
  XLS_ASSIGN_OR_RETURN(auto* type, ParseTypeAnnotation(bindings));
  return module_->Make<ChannelDecl>(
      Span(channel.span().start(), type->span().limit()), type);
}

absl::StatusOr<std::vector<Expr*>> Parser::ParseDims(Bindings* bindings,
                                                     Pos* limit_pos) {
  XLS_ASSIGN_OR_RETURN(Token obrack, PopTokenOrError(TokenKind::kOBrack));
  XLS_ASSIGN_OR_RETURN(Expr * dim, ParseTernaryExpression(bindings));
  std::vector<Expr*> dims = {dim};
  const char* const kContext = "at end of type dimensions";
  XLS_RETURN_IF_ERROR(
      DropTokenOrError(TokenKind::kCBrack, &obrack, kContext, limit_pos));
  while (true) {
    XLS_ASSIGN_OR_RETURN(bool dropped_obrack,
                         TryDropToken(TokenKind::kOBrack, limit_pos));
    if (!dropped_obrack) {
      break;
    }
    XLS_ASSIGN_OR_RETURN(Expr * dim, ParseTernaryExpression(bindings));
    dims.push_back(dim);
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCBrack, /*start=*/&obrack,
                                         /*context=*/kContext,
                                         /*limit_pos=*/limit_pos));
  }
  return dims;
}

absl::StatusOr<TypeRef*> Parser::ParseModTypeRef(Bindings* bindings,
                                                 const Token& start_tok) {
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kDoubleColon));
  XLS_ASSIGN_OR_RETURN(
      BoundNode bn,
      bindings->ResolveNodeOrError(*start_tok.GetValue(), start_tok.span()));
  if (!absl::holds_alternative<Import*>(bn)) {
    return ParseErrorStatus(
        start_tok.span(),
        absl::StrFormat("Expected module for module-reference; got %s",
                        ToAstNode(bn)->ToString()));
  }
  XLS_ASSIGN_OR_RETURN(NameRef * subject, ParseNameRef(bindings, &start_tok));
  XLS_ASSIGN_OR_RETURN(Token type_name,
                       PopTokenOrError(TokenKind::kIdentifier));
  const Span span(start_tok.span().start(), type_name.span().limit());
  ColonRef* mod_ref =
      module_->Make<ColonRef>(span, subject, *type_name.GetValue());
  std::string composite =
      absl::StrFormat("%s::%s", *start_tok.GetValue(), *type_name.GetValue());
  return module_->Make<TypeRef>(span, /*text=*/composite, mod_ref);
}

absl::StatusOr<Let*> Parser::ParseLet(Bindings* bindings) {
  Bindings new_bindings(bindings);
  XLS_ASSIGN_OR_RETURN(Token start_tok, PopToken());
  bool const_;
  if (start_tok.IsKeyword(Keyword::kLet)) {
    const_ = false;
  } else if (start_tok.IsKeyword(Keyword::kConst)) {
    const_ = true;
  } else {
    return ParseErrorStatus(
        start_tok.span(),
        absl::StrFormat("Expected 'let' or 'const'; got %s @ %s",
                        start_tok.ToErrorString(),
                        start_tok.span().ToString()));
  }

  NameDef* name_def = nullptr;
  NameDefTree* name_def_tree;
  XLS_ASSIGN_OR_RETURN(bool peek_is_oparen, PeekTokenIs(TokenKind::kOParen));
  if (peek_is_oparen) {  // Destructuring binding.
    XLS_ASSIGN_OR_RETURN(name_def_tree, ParseNameDefTree(&new_bindings));
  } else {
    XLS_ASSIGN_OR_RETURN(name_def, ParseNameDef(&new_bindings));
    name_def_tree = module_->Make<NameDefTree>(name_def->span(), name_def);
  }

  XLS_ASSIGN_OR_RETURN(bool dropped_colon, TryDropToken(TokenKind::kColon));
  TypeAnnotation* annotated_type = nullptr;
  if (dropped_colon) {
    XLS_ASSIGN_OR_RETURN(annotated_type, ParseTypeAnnotation(bindings));
  }

  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kEquals));
  XLS_ASSIGN_OR_RETURN(Expr * rhs, ParseExpression(bindings));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kSemi));
  ConstantDef* const_def = nullptr;
  if (const_ && name_def != nullptr) {
    Span span(name_def->span().start(), rhs->span().limit());
    const_def = module_->Make<ConstantDef>(
        span, name_def, rhs, /*is_public=*/false, /*is_local=*/true);
    new_bindings.Add(name_def->identifier(), const_def);
    name_def->set_definer(const_def);
  }
  XLS_ASSIGN_OR_RETURN(Expr * body, ParseExpression(&new_bindings));
  Span span(start_tok.span().start(), GetPos());
  return module_->Make<Let>(span, name_def_tree, annotated_type, rhs, body,
                            const_def);
}

absl::StatusOr<For*> Parser::ParseFor(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(Token for_, PopKeywordOrError(Keyword::kFor));

  Bindings for_bindings(bindings);
  XLS_ASSIGN_OR_RETURN(NameDefTree * names, ParseNameDefTree(&for_bindings));
  XLS_ASSIGN_OR_RETURN(bool peek_is_colon, PeekTokenIs(TokenKind::kColon));
  TypeAnnotation* type = nullptr;
  if (peek_is_colon) {
    XLS_RETURN_IF_ERROR(
        DropTokenOrError(TokenKind::kColon, nullptr,
                         "Expect type annotation on for-loop values."));
    XLS_ASSIGN_OR_RETURN(type, ParseTypeAnnotation(&for_bindings));
  }
  XLS_RETURN_IF_ERROR(DropKeywordOrError(Keyword::kIn));
  XLS_ASSIGN_OR_RETURN(Expr * iterable, ParseExpression(bindings));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOBrace));
  XLS_ASSIGN_OR_RETURN(Expr * body, ParseExpression(&for_bindings));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCBrace));
  XLS_RETURN_IF_ERROR(DropTokenOrError(
      TokenKind::kOParen, &for_,
      "Need an initial accumulator value to start the for loop."));

  // We must be sure to use the outer bindings when parsing the init
  // expression, since the for loop bindings haven't happened yet (no loop
  // trips have iterated when the init value is evaluated).
  XLS_ASSIGN_OR_RETURN(Expr * init, ParseExpression(bindings));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCParen));
  return module_->Make<For>(Span(for_.span().limit(), GetPos()), names, type,
                            iterable, body, init);
}

absl::StatusOr<EnumDef*> Parser::ParseEnumDef(bool is_public,
                                              Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(Token enum_tok, PopKeywordOrError(Keyword::kEnum));
  XLS_ASSIGN_OR_RETURN(NameDef * name_def, ParseNameDef(bindings));
  XLS_RETURN_IF_ERROR(
      DropTokenOrError(TokenKind::kColon, nullptr,
                       "enum requires a ': type' annotation to indicate "
                       "enum's underlying type."));
  XLS_ASSIGN_OR_RETURN(TypeAnnotation * type_annotation,
                       ParseTypeAnnotation(bindings));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOBrace));
  Bindings enum_bindings(bindings);

  auto parse_enum_entry = [this, &enum_bindings,
                           type_annotation]() -> absl::StatusOr<EnumMember> {
    XLS_ASSIGN_OR_RETURN(NameDef * name_def, ParseNameDef(&enum_bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kEquals));
    XLS_ASSIGN_OR_RETURN(Expr * expr, ParseExpression(&enum_bindings));
    // Propagate type annotation to un-annotated enum entries -- this is a
    // convenience until we have proper unifying type inference.
    if (auto* number = dynamic_cast<Number*>(expr); number != nullptr) {
      if (number->type_annotation() == nullptr) {
        number->set_type_annotation(type_annotation);
      } else {
        return ParseErrorStatus(
            number->type_annotation()->span(),
            "A type is annotated on this enum value, but the enum defines a "
            "type, so this is not necessary: please remove it.");
      }
    }
    return EnumMember{name_def, expr};
  };

  XLS_ASSIGN_OR_RETURN(
      std::vector<EnumMember> entries,
      ParseCommaSeq<EnumMember>(parse_enum_entry, TokenKind::kCBrace));
  auto* enum_def = module_->Make<EnumDef>(enum_tok.span(), name_def,
                                          type_annotation, entries, is_public);
  bindings->Add(name_def->identifier(), enum_def);
  name_def->set_definer(enum_def);
  return enum_def;
}

absl::StatusOr<TypeAnnotation*> Parser::MakeBuiltinTypeAnnotation(
    const Span& span, const Token& tok, absl::Span<Expr* const> dims) {
  XLS_ASSIGN_OR_RETURN(BuiltinType builtin_type, TokenToBuiltinType(tok));
  TypeAnnotation* elem_type =
      module_->Make<BuiltinTypeAnnotation>(tok.span(), builtin_type);
  for (Expr* dim : dims) {
    elem_type = module_->Make<ArrayTypeAnnotation>(span, elem_type, dim);
  }
  return elem_type;
}

absl::StatusOr<TypeAnnotation*> Parser::MakeTypeRefTypeAnnotation(
    const Span& span, TypeRef* type_ref, std::vector<Expr*> dims,
    std::vector<Expr*> parametrics) {
  TypeAnnotation* elem_type = module_->Make<TypeRefTypeAnnotation>(
      span, type_ref, std::move(parametrics));
  for (Expr* dim : dims) {
    elem_type = module_->Make<ArrayTypeAnnotation>(span, elem_type, dim);
  }
  return elem_type;
}

absl::StatusOr<Expr*> Parser::ParseCastOrStructInstance(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(TypeAnnotation * type, ParseTypeAnnotation(bindings));
  XLS_ASSIGN_OR_RETURN(bool peek_is_colon, PeekTokenIs(TokenKind::kColon));
  if (peek_is_colon) {
    return ParseCast(bindings, type);
  }
  return ParseStructInstance(bindings, type);
}

absl::StatusOr<absl::variant<NameDef*, WildcardPattern*>>
Parser::ParseNameDefOrWildcard(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(absl::optional<Token> tok, TryPopIdentifierToken("_"));
  if (tok) {
    return module_->Make<WildcardPattern>(tok->span());
  }
  return ParseNameDef(bindings);
}

absl::StatusOr<Param*> Parser::ParseParam(Bindings* bindings) {
  XLS_ASSIGN_OR_RETURN(NameDef * name, ParseNameDef(bindings));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kColon));
  XLS_ASSIGN_OR_RETURN(TypeAnnotation * type, ParseTypeAnnotation(bindings));
  auto* param = module_->Make<Param>(name, type);
  name->set_definer(param);
  return param;
}

absl::StatusOr<Number*> Parser::ParseNumber(Bindings* bindings) {
  // Token pointers are not guaranteed to persist through Peek/Pop calls, so we
  // need to make a copy for logging below.
  XLS_ASSIGN_OR_RETURN(const Token* peek_tmp, PeekToken());
  Token peek = *peek_tmp;

  if (peek.kind() == TokenKind::kNumber ||
      peek.kind() == TokenKind::kCharacter ||
      peek.IsKeywordIn({Keyword::kTrue, Keyword::kFalse})) {
    return TokenToNumber(PopTokenOrDie());
  }

  // Numbers can also be given as u32:4 -- last ditch effort to parse one of
  // those.
  absl::StatusOr<Expr*> cast = ParseCast(bindings);
  if (cast.ok() && dynamic_cast<Number*>(cast.value()) != nullptr) {
    return dynamic_cast<Number*>(cast.value());
  }

  return ParseErrorStatus(
      peek.span(),
      absl::StrFormat("Expected number; got %s @ %s",
                      TokenKindToString(peek.kind()), peek.span().ToString()));
}

absl::StatusOr<StructDef*> Parser::ParseStruct(bool is_public,
                                               Bindings* bindings) {
  const Pos start_pos = GetPos();
  XLS_RETURN_IF_ERROR(DropKeywordOrError(Keyword::kStruct));

  XLS_ASSIGN_OR_RETURN(NameDef * name_def, ParseNameDef(bindings));

  XLS_ASSIGN_OR_RETURN(bool dropped_oangle, TryDropToken(TokenKind::kOAngle));
  std::vector<ParametricBinding*> parametric_bindings;
  if (dropped_oangle) {
    XLS_ASSIGN_OR_RETURN(parametric_bindings,
                         ParseParametricBindings(bindings));
  }

  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOBrace));

  using StructMember = std::pair<NameDef*, TypeAnnotation*>;
  auto parse_struct_member = [this,
                              bindings]() -> absl::StatusOr<StructMember> {
    XLS_ASSIGN_OR_RETURN(Token tok, PopTokenOrError(TokenKind::kIdentifier));
    XLS_ASSIGN_OR_RETURN(NameDef * name_def, TokenToNameDef(tok));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kColon));
    XLS_ASSIGN_OR_RETURN(TypeAnnotation * type, ParseTypeAnnotation(bindings));
    return StructMember{name_def, type};
  };

  XLS_ASSIGN_OR_RETURN(
      std::vector<StructMember> members,
      ParseCommaSeq<StructMember>(parse_struct_member, TokenKind::kCBrace));
  Span span(start_pos, GetPos());
  auto* struct_def = module_->Make<StructDef>(
      span, name_def, parametric_bindings, members, is_public);
  bindings->Add(name_def->identifier(), struct_def);
  return struct_def;
}

absl::StatusOr<NameDefTree*> Parser::ParseTuplePattern(const Pos& start_pos,
                                                       Bindings* bindings) {
  std::vector<NameDefTree*> members;
  bool must_end = false;
  while (true) {
    XLS_ASSIGN_OR_RETURN(bool dropped_cparen, TryDropToken(TokenKind::kCParen));
    if (dropped_cparen) {
      break;
    }
    if (must_end) {
      XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCParen));
      break;
    }
    XLS_ASSIGN_OR_RETURN(NameDefTree * pattern, ParsePattern(bindings));
    members.push_back(pattern);
    XLS_ASSIGN_OR_RETURN(bool dropped_comma, TryDropToken(TokenKind::kComma));
    must_end = !dropped_comma;
  }
  Span span(start_pos, GetPos());
  return module_->Make<NameDefTree>(span, std::move(members));
}

absl::StatusOr<Expr*> Parser::ParseBlockExpression(Bindings* bindings) {
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOBrace));
  XLS_ASSIGN_OR_RETURN(Expr * e, ParseExpression(bindings));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCBrace));
  return e;
}

absl::StatusOr<Expr*> Parser::ParseParenthesizedExpr(Bindings* bindings) {
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOParen));
  XLS_ASSIGN_OR_RETURN(Expr * e, ParseExpression(bindings));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCParen));
  return e;
}

absl::StatusOr<std::vector<ParametricBinding*>> Parser::ParseParametricBindings(
    Bindings* bindings) {
  auto parse_parametric_binding =
      [this, bindings]() -> absl::StatusOr<ParametricBinding*> {
    XLS_ASSIGN_OR_RETURN(NameDef * name_def, ParseNameDef(bindings));
    XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kColon));
    XLS_ASSIGN_OR_RETURN(TypeAnnotation * type, ParseTypeAnnotation(bindings));
    XLS_ASSIGN_OR_RETURN(bool dropped_equals, TryDropToken(TokenKind::kEquals));
    Expr* expr = nullptr;
    if (dropped_equals) {
      XLS_ASSIGN_OR_RETURN(expr, ParseExpression(bindings));
    }
    return module_->Make<ParametricBinding>(name_def, type, expr);
  };
  return ParseCommaSeq<ParametricBinding*>(parse_parametric_binding,
                                           TokenKind::kCAngle);
}

absl::StatusOr<std::vector<Expr*>> Parser::ParseParametrics(
    Bindings* bindings) {
  // We need two levels of bindings - one per-parse-parametrics call and one at
  // top-level.
  Transaction txn(this, bindings);
  auto cleanup = absl::MakeCleanup([&txn]() { txn.Rollback(); });

  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOAngle));
  auto parse_parametric = [this, &txn]() -> absl::StatusOr<Expr*> {
    XLS_ASSIGN_OR_RETURN(const Token* peek, PeekToken());
    if (peek->kind() == TokenKind::kOBrace) {
      // Ternary expressions are the first below the let/for/while set.
      Transaction sub_txn(this, txn.bindings());
      auto cleanup = absl::MakeCleanup([&sub_txn]() { sub_txn.Rollback(); });

      XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOBrace));
      XLS_ASSIGN_OR_RETURN(Expr * expr,
                           ParseTernaryExpression(sub_txn.bindings()));
      XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCBrace));

      sub_txn.CommitAndCancelCleanup(&cleanup);
      return expr;
    }

    auto status_or_literal = TryOrRollback<Number*>(
        txn.bindings(),
        [this](Bindings* bindings) { return ParseNumber(bindings); });
    if (status_or_literal.ok()) {
      return status_or_literal;
    }

    auto status_or_ref = TryOrRollback<absl::variant<NameRef*, ColonRef*>>(
        txn.bindings(),
        [this](Bindings* bindings) { return ParseNameOrColonRef(bindings); });
    XLS_ASSIGN_OR_RETURN(auto ref, status_or_ref);
    if (absl::holds_alternative<NameRef*>(ref)) {
      return absl::get<NameRef*>(ref);
    }

    return absl::get<ColonRef*>(ref);
  };

  auto status_or_exprs =
      ParseCommaSeq<Expr*>(parse_parametric, TokenKind::kCAngle);
  if (status_or_exprs.ok()) {
    txn.CommitAndCancelCleanup(&cleanup);
  }
  return status_or_exprs;
}

absl::StatusOr<TestFunction*> Parser::ParseTestFunction(
    Bindings* bindings, const Span& directive_span) {
  XLS_ASSIGN_OR_RETURN(Function * f,
                       ParseFunctionInternal(/*is_public=*/false, bindings));
  if (absl::optional<ModuleMember*> member =
          module_->FindMemberWithName(f->identifier())) {
    return ParseErrorStatus(
        directive_span,
        absl::StrFormat(
            "Test function '%s' has same name as module member @ %s",
            f->identifier(), ToAstNode(**member)->GetSpan()->ToString()));
  }
  return module_->Make<TestFunction>(f);
}

absl::StatusOr<TestProc*> Parser::ParseTestProc(
    Bindings* bindings, const std::vector<Expr*>& initial_values) {
  XLS_ASSIGN_OR_RETURN(Proc * p, ParseProc(/*is_public=*/false, bindings));
  if (absl::optional<ModuleMember*> member =
          module_->FindMemberWithName(p->identifier())) {
    return ParseErrorStatus(
        p->span(),
        absl::StrFormat("Test proc '%s' has same name as module member @ %s",
                        p->identifier(),
                        ToAstNode(**member)->GetSpan()->ToString()));
  }

  // Verify no state or config args
  return module_->Make<TestProc>(p, initial_values);
}

absl::Status Parser::ParseConfig(const Span& directive_span) {
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kOParen));
  XLS_ASSIGN_OR_RETURN(std::string config_name, PopIdentifierOrError());
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kEquals));
  XLS_ASSIGN_OR_RETURN(Token config_value,
                       PopTokenOrError(TokenKind::kKeyword));
  XLS_RETURN_IF_ERROR(DropTokenOrError(TokenKind::kCParen));
  return ParseErrorStatus(
      directive_span,
      absl::StrFormat("Unknown configuration key in directive: '%s'",
                      config_name));
}

const Span& GetSpan(const absl::variant<NameDef*, WildcardPattern*>& v) {
  if (absl::holds_alternative<NameDef*>(v)) {
    return absl::get<NameDef*>(v)->span();
  }
  return absl::get<WildcardPattern*>(v)->span();
}

}  // namespace xls::dslx
