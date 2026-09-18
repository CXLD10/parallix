#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "parallix/token.h"

namespace parallix {

// Thrown on a character the lexer cannot form into any valid token.
class LexError : public std::runtime_error {
 public:
  explicit LexError(const std::string& message) : std::runtime_error(message) {}
};

class Lexer {
 public:
  explicit Lexer(std::string source);

  // Tokenizes the whole source and returns it as a stream ending in
  // END_OF_FILE. Throws LexError on an unrecognized character.
  std::vector<Token> Tokenize();

 private:
  std::string source_;
  size_t pos_ = 0;
  int line_ = 1;
  int column_ = 1;

  bool AtEnd() const;
  char Peek(size_t offset = 0) const;
  char Advance();
  bool Match(char expected);

  void SkipWhitespaceAndComments();
  Token NextToken();
  Token MakeToken(TokenKind kind, const std::string& lexeme, int line, int column);
  Token LexIdentifierOrKeyword();
  Token LexNumber();
};

}  // namespace parallix
