#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "parallix/ast.h"
#include "parallix/token.h"

namespace parallix {

// Thrown on a token sequence that does not match the grammar in specs/02.
// This is a syntax error, distinct from a semantic error (see sema.h) — T5
// and T6 parse successfully and only fail later, in semantic analysis.
class ParseError : public std::runtime_error {
 public:
  explicit ParseError(const std::string& message) : std::runtime_error(message) {}
};

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens);

  // Parses the full token stream per specs/02's grammar. Throws ParseError
  // on the first token sequence that cannot be reduced by the grammar.
  Program ParseProgram();

 private:
  std::vector<Token> tokens_;
  size_t pos_ = 0;

  const Token& Peek(size_t offset = 0) const;
  const Token& Advance();
  bool Check(TokenKind kind) const;
  bool Match(TokenKind kind);
  const Token& Expect(TokenKind kind, const std::string& context);
  [[noreturn]] void Error(const std::string& message) const;

  KernelDecl ParseKernel();
  std::vector<Param> ParseParams();
  Param ParseParam();
  Param ParseRet();
  TypeNode ParseType();
  Dim ParseDim();

  StmtPtr ParseStmt();
  StmtPtr ParseLoop();
  StmtPtr ParseReduction();
  StmtPtr ParseAssign();
  bool IsLoopMode(TokenKind kind) const;
  LoopMode ToLoopMode(TokenKind kind) const;

  ExprPtr ParseExpr();
  ExprPtr ParseTerm();
  ExprPtr ParseFactor();
  std::unique_ptr<VarRefExpr> ParseLValue();
};

}  // namespace parallix
