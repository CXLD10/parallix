#pragma once

#include <string>

namespace parallix {

enum class TokenKind {
  // Keywords
  KERNEL,
  AUTO,
  PARALLEL,
  VECTORIZE,
  SEQ,
  REDUCE,
  IN,

  // Type keywords
  F32,
  F64,
  I32,
  I64,
  SELECT,

  // Reduction-operator keywords (also usable where 'min'/'max' are named)
  MIN,
  MAX,

  // Literals / identifiers
  IDENT,
  INT_LITERAL,
  FLOAT_LITERAL,

  // Operators
  ASSIGN,       // =
  EQ,           // ==
  NE,           // !=
  LT,           // <
  LE,           // <=
  GT,           // >
  GE,           // >=
  PLUS_ASSIGN,  // +=
  PLUS,         // +
  MINUS,        // -
  STAR,         // *
  SLASH,        // /
  DOTDOT,       // ..
  COLON,        // :
  ARROW,        // ->

  // Punctuation
  LPAREN,
  RPAREN,
  LBRACE,
  RBRACE,
  LBRACKET,
  RBRACKET,
  COMMA,

  END_OF_FILE,
};

const char* ToString(TokenKind kind);

struct Token {
  TokenKind kind;
  std::string lexeme;
  int line;
  int column;
};

}  // namespace parallix
