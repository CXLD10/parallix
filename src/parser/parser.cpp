#include "parallix/parser.h"

namespace parallix {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

const Token& Parser::Peek(size_t offset) const {
  size_t idx = pos_ + offset;
  if (idx >= tokens_.size()) return tokens_.back();  // END_OF_FILE
  return tokens_[idx];
}

const Token& Parser::Advance() {
  const Token& tok = Peek();
  if (pos_ < tokens_.size() - 1) pos_++;
  return tok;
}

bool Parser::Check(TokenKind kind) const { return Peek().kind == kind; }

bool Parser::Match(TokenKind kind) {
  if (!Check(kind)) return false;
  Advance();
  return true;
}

const Token& Parser::Expect(TokenKind kind, const std::string& context) {
  if (!Check(kind)) {
    Error("expected " + std::string(ToString(kind)) + " " + context + ", but found " +
          ToString(Peek().kind) + " ('" + Peek().lexeme + "') at line " +
          std::to_string(Peek().line));
  }
  return Advance();
}

void Parser::Error(const std::string& message) const { throw ParseError(message); }

Program Parser::ParseProgram() {
  Program program;
  if (Check(TokenKind::END_OF_FILE)) {
    Error("expected at least one kernel declaration, but found end of file");
  }
  while (!Check(TokenKind::END_OF_FILE)) {
    program.kernels.push_back(ParseKernel());
  }
  return program;
}

KernelDecl Parser::ParseKernel() {
  KernelDecl kernel;
  const Token& kw = Expect(TokenKind::KERNEL, "to start a kernel declaration");
  kernel.line = kw.line;
  kernel.column = kw.column;
  kernel.name = Expect(TokenKind::IDENT, "as the kernel name").lexeme;
  Expect(TokenKind::LPAREN, "to open the kernel parameter list");
  if (!Check(TokenKind::RPAREN)) {
    kernel.params = ParseParams();
  }
  Expect(TokenKind::RPAREN, "to close the kernel parameter list");
  Expect(TokenKind::ARROW, "before the kernel's return binding");
  kernel.ret = ParseRet();
  Expect(TokenKind::LBRACE, "to open the kernel body");
  if (Check(TokenKind::RBRACE)) {
    Error("kernel '" + kernel.name + "' has an empty body; a kernel requires at least one statement");
  }
  while (!Check(TokenKind::RBRACE)) {
    kernel.body.push_back(ParseStmt());
  }
  Expect(TokenKind::RBRACE, "to close the kernel body");
  return kernel;
}

std::vector<Param> Parser::ParseParams() {
  std::vector<Param> params;
  params.push_back(ParseParam());
  while (Match(TokenKind::COMMA)) {
    params.push_back(ParseParam());
  }
  return params;
}

Param Parser::ParseParam() {
  Param param;
  param.name = Expect(TokenKind::IDENT, "as a parameter name").lexeme;
  Expect(TokenKind::COLON, "between the parameter name and its type");
  param.type = ParseType();
  return param;
}

Param Parser::ParseRet() {
  Param ret;
  ret.name = Expect(TokenKind::IDENT, "as the return binding name").lexeme;
  Expect(TokenKind::COLON, "between the return binding name and its type");
  ret.type = ParseType();
  return ret;
}

TypeNode Parser::ParseType() {
  TypeNode type;
  if (Check(TokenKind::F32)) {
    type.base = TypeBase::F32;
  } else if (Check(TokenKind::F64)) {
    type.base = TypeBase::F64;
  } else if (Check(TokenKind::I32)) {
    type.base = TypeBase::I32;
  } else {
    Error("expected a base type (f32, f64, i32), but found " + std::string(ToString(Peek().kind)) +
          " ('" + Peek().lexeme + "') at line " + std::to_string(Peek().line));
  }
  Advance();

  if (Match(TokenKind::LBRACKET)) {
    type.dims.push_back(ParseDim());
    while (Match(TokenKind::COMMA)) {
      type.dims.push_back(ParseDim());
    }
    Expect(TokenKind::RBRACKET, "to close the array shape");
  }
  return type;
}

Dim Parser::ParseDim() {
  Dim dim;
  if (Check(TokenKind::IDENT)) {
    dim.is_ident = true;
    dim.ident = Advance().lexeme;
  } else if (Check(TokenKind::INT_LITERAL)) {
    dim.is_ident = false;
    dim.int_value = std::stoll(Advance().lexeme);
  } else {
    Error("expected an array dimension (identifier or integer), but found " +
          std::string(ToString(Peek().kind)) + " ('" + Peek().lexeme + "') at line " +
          std::to_string(Peek().line));
  }
  return dim;
}

bool Parser::IsLoopMode(TokenKind kind) const {
  return kind == TokenKind::AUTO || kind == TokenKind::PARALLEL ||
         kind == TokenKind::VECTORIZE || kind == TokenKind::SEQ;
}

LoopMode Parser::ToLoopMode(TokenKind kind) const {
  switch (kind) {
    case TokenKind::AUTO: return LoopMode::AUTO;
    case TokenKind::PARALLEL: return LoopMode::PARALLEL;
    case TokenKind::VECTORIZE: return LoopMode::VECTORIZE;
    case TokenKind::SEQ: return LoopMode::SEQ;
    default: Error("internal error: not a loop mode token");
  }
}

StmtPtr Parser::ParseStmt() {
  if (IsLoopMode(Peek().kind)) return ParseLoop();
  if (Check(TokenKind::REDUCE)) return ParseReduction();
  if (Check(TokenKind::IDENT)) return ParseAssign();
  Error("expected a statement (loop, reduction, or assignment), but found " +
        std::string(ToString(Peek().kind)) + " ('" + Peek().lexeme + "') at line " +
        std::to_string(Peek().line));
}

StmtPtr Parser::ParseLoop() {
  auto loop = std::make_unique<LoopStmt>();
  const Token& mode_tok = Advance();
  loop->line = mode_tok.line;
  loop->column = mode_tok.column;
  loop->mode = ToLoopMode(mode_tok.kind);
  loop->index_var = Expect(TokenKind::IDENT, "as the loop induction variable").lexeme;
  Expect(TokenKind::IN, "after the loop induction variable");
  loop->lower = ParseExpr();
  Expect(TokenKind::DOTDOT, "between the loop's lower and upper bounds");
  loop->upper = ParseExpr();
  Expect(TokenKind::LBRACE, "to open the loop body");
  if (Check(TokenKind::RBRACE)) {
    Error("loop over '" + loop->index_var + "' has an empty body");
  }
  while (!Check(TokenKind::RBRACE)) {
    loop->body.push_back(ParseStmt());
  }
  Expect(TokenKind::RBRACE, "to close the loop body");
  return loop;
}

StmtPtr Parser::ParseReduction() {
  auto reduction = std::make_unique<ReductionStmt>();
  const Token& kw = Expect(TokenKind::REDUCE, "to start a reduction");
  reduction->line = kw.line;
  reduction->column = kw.column;
  Expect(TokenKind::LPAREN, "to open the reduction operator");
  if (Check(TokenKind::PLUS)) {
    reduction->op = ReduceOp::ADD;
  } else if (Check(TokenKind::STAR)) {
    reduction->op = ReduceOp::MUL;
  } else if (Check(TokenKind::MIN)) {
    reduction->op = ReduceOp::MIN;
  } else if (Check(TokenKind::MAX)) {
    reduction->op = ReduceOp::MAX;
  } else {
    Error("expected a reduction operator (+, *, min, max), but found " +
          std::string(ToString(Peek().kind)) + " ('" + Peek().lexeme + "') at line " +
          std::to_string(Peek().line));
  }
  Advance();
  Expect(TokenKind::RPAREN, "to close the reduction operator");
  reduction->index_var = Expect(TokenKind::IDENT, "as the reduction induction variable").lexeme;
  Expect(TokenKind::IN, "after the reduction induction variable");
  reduction->lower = ParseExpr();
  Expect(TokenKind::DOTDOT, "between the reduction's lower and upper bounds");
  reduction->upper = ParseExpr();
  Expect(TokenKind::LBRACE, "to open the reduction body");
  if (Check(TokenKind::RBRACE)) {
    Error("reduction over '" + reduction->index_var + "' has an empty body");
  }
  while (!Check(TokenKind::RBRACE)) {
    reduction->body.push_back(ParseStmt());
  }
  Expect(TokenKind::RBRACE, "to close the reduction body");
  return reduction;
}

StmtPtr Parser::ParseAssign() {
  auto assign = std::make_unique<AssignStmt>();
  const Token& start = Peek();
  assign->line = start.line;
  assign->column = start.column;
  assign->lvalue = ParseLValue();

  if (Check(TokenKind::ASSIGN)) {
    assign->op = AssignOp::SET;
  } else if (Check(TokenKind::PLUS_ASSIGN)) {
    assign->op = AssignOp::ADD_SET;
  } else {
    Error("expected '=' or '+=' after lvalue '" + assign->lvalue->name + "', but found " +
          std::string(ToString(Peek().kind)) + " ('" + Peek().lexeme + "') at line " +
          std::to_string(Peek().line));
  }
  Advance();
  assign->rhs = ParseExpr();
  return assign;
}

std::unique_ptr<VarRefExpr> Parser::ParseLValue() {
  auto ref = std::make_unique<VarRefExpr>();
  const Token& name_tok = Expect(TokenKind::IDENT, "as a variable or array name");
  ref->line = name_tok.line;
  ref->column = name_tok.column;
  ref->name = name_tok.lexeme;
  if (Match(TokenKind::LBRACKET)) {
    ref->subscripts.push_back(ParseExpr());
    while (Match(TokenKind::COMMA)) {
      ref->subscripts.push_back(ParseExpr());
    }
    Expect(TokenKind::RBRACKET, "to close the subscript list for '" + ref->name + "'");
  }
  return ref;
}

ExprPtr Parser::ParseExpr() {
  ExprPtr lhs = ParseTerm();
  while (Check(TokenKind::PLUS) || Check(TokenKind::MINUS)) {
    const Token& op_tok = Advance();
    auto bin = std::make_unique<BinaryExpr>();
    bin->line = op_tok.line;
    bin->column = op_tok.column;
    bin->op = (op_tok.kind == TokenKind::PLUS) ? BinaryOp::ADD : BinaryOp::SUB;
    bin->lhs = std::move(lhs);
    bin->rhs = ParseTerm();
    lhs = std::move(bin);
  }
  return lhs;
}

ExprPtr Parser::ParseTerm() {
  ExprPtr lhs = ParseFactor();
  while (Check(TokenKind::STAR) || Check(TokenKind::SLASH)) {
    const Token& op_tok = Advance();
    auto bin = std::make_unique<BinaryExpr>();
    bin->line = op_tok.line;
    bin->column = op_tok.column;
    bin->op = (op_tok.kind == TokenKind::STAR) ? BinaryOp::MUL : BinaryOp::DIV;
    bin->lhs = std::move(lhs);
    bin->rhs = ParseFactor();
    lhs = std::move(bin);
  }
  return lhs;
}

// factor := IDENT | INT | lvalue | '(' expr ')'
// The IDENT and lvalue alternatives collapse into one case here: a bare
// IDENT is just a VarRefExpr with no subscripts.
ExprPtr Parser::ParseFactor() {
  if (Check(TokenKind::IDENT)) {
    return ParseLValue();
  }
  if (Check(TokenKind::INT_LITERAL)) {
    const Token& tok = Advance();
    auto lit = std::make_unique<IntLiteralExpr>();
    lit->line = tok.line;
    lit->column = tok.column;
    lit->value = std::stoll(tok.lexeme);
    return lit;
  }
  if (Match(TokenKind::LPAREN)) {
    ExprPtr inner = ParseExpr();
    Expect(TokenKind::RPAREN, "to close the parenthesized expression");
    return inner;
  }
  Error("expected an identifier, integer literal, or '(', but found " +
        std::string(ToString(Peek().kind)) + " ('" + Peek().lexeme + "') at line " +
        std::to_string(Peek().line));
}

}  // namespace parallix
