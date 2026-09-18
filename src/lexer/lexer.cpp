#include "parallix/lexer.h"

#include <cctype>
#include <unordered_map>

namespace parallix {

const char* ToString(TokenKind kind) {
  switch (kind) {
    case TokenKind::KERNEL: return "KERNEL";
    case TokenKind::AUTO: return "AUTO";
    case TokenKind::PARALLEL: return "PARALLEL";
    case TokenKind::VECTORIZE: return "VECTORIZE";
    case TokenKind::SEQ: return "SEQ";
    case TokenKind::REDUCE: return "REDUCE";
    case TokenKind::IN: return "IN";
    case TokenKind::F32: return "F32";
    case TokenKind::F64: return "F64";
    case TokenKind::I32: return "I32";
    case TokenKind::MIN: return "MIN";
    case TokenKind::MAX: return "MAX";
    case TokenKind::IDENT: return "IDENT";
    case TokenKind::INT_LITERAL: return "INT";
    case TokenKind::FLOAT_LITERAL: return "FLOAT";
    case TokenKind::ASSIGN: return "ASSIGN";
    case TokenKind::PLUS_ASSIGN: return "PLUS_ASSIGN";
    case TokenKind::PLUS: return "PLUS";
    case TokenKind::MINUS: return "MINUS";
    case TokenKind::STAR: return "STAR";
    case TokenKind::SLASH: return "SLASH";
    case TokenKind::DOTDOT: return "DOTDOT";
    case TokenKind::COLON: return "COLON";
    case TokenKind::ARROW: return "ARROW";
    case TokenKind::LPAREN: return "LPAREN";
    case TokenKind::RPAREN: return "RPAREN";
    case TokenKind::LBRACE: return "LBRACE";
    case TokenKind::RBRACE: return "RBRACE";
    case TokenKind::LBRACKET: return "LBRACKET";
    case TokenKind::RBRACKET: return "RBRACKET";
    case TokenKind::COMMA: return "COMMA";
    case TokenKind::END_OF_FILE: return "EOF";
  }
  return "UNKNOWN";
}

namespace {
const std::unordered_map<std::string, TokenKind>& Keywords() {
  static const std::unordered_map<std::string, TokenKind> kKeywords = {
      {"kernel", TokenKind::KERNEL},       {"auto", TokenKind::AUTO},
      {"parallel", TokenKind::PARALLEL},   {"vectorize", TokenKind::VECTORIZE},
      {"seq", TokenKind::SEQ},             {"reduce", TokenKind::REDUCE},
      {"in", TokenKind::IN},               {"f32", TokenKind::F32},
      {"f64", TokenKind::F64},             {"i32", TokenKind::I32},
      {"min", TokenKind::MIN},             {"max", TokenKind::MAX},
  };
  return kKeywords;
}
}  // namespace

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

bool Lexer::AtEnd() const { return pos_ >= source_.size(); }

char Lexer::Peek(size_t offset) const {
  size_t idx = pos_ + offset;
  if (idx >= source_.size()) return '\0';
  return source_[idx];
}

char Lexer::Advance() {
  char c = source_[pos_++];
  if (c == '\n') {
    line_++;
    column_ = 1;
  } else {
    column_++;
  }
  return c;
}

bool Lexer::Match(char expected) {
  if (AtEnd() || Peek() != expected) return false;
  Advance();
  return true;
}

Token Lexer::MakeToken(TokenKind kind, const std::string& lexeme, int line, int column) {
  return Token{kind, lexeme, line, column};
}

void Lexer::SkipWhitespaceAndComments() {
  for (;;) {
    if (AtEnd()) return;
    char c = Peek();
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      Advance();
    } else if (c == '/' && Peek(1) == '/') {
      while (!AtEnd() && Peek() != '\n') Advance();
    } else {
      return;
    }
  }
}

Token Lexer::LexIdentifierOrKeyword() {
  int line = line_;
  int column = column_;
  size_t start = pos_;
  while (!AtEnd() && (std::isalnum(static_cast<unsigned char>(Peek())) || Peek() == '_')) {
    Advance();
  }
  std::string text = source_.substr(start, pos_ - start);
  const auto& keywords = Keywords();
  auto it = keywords.find(text);
  TokenKind kind = (it != keywords.end()) ? it->second : TokenKind::IDENT;
  return MakeToken(kind, text, line, column);
}

Token Lexer::LexNumber() {
  int line = line_;
  int column = column_;
  size_t start = pos_;
  while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek()))) Advance();

  bool is_float = false;
  if (Peek() == '.' && std::isdigit(static_cast<unsigned char>(Peek(1)))) {
    is_float = true;
    Advance();  // consume '.'
    while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek()))) Advance();
  }

  std::string text = source_.substr(start, pos_ - start);
  return MakeToken(is_float ? TokenKind::FLOAT_LITERAL : TokenKind::INT_LITERAL, text, line,
                    column);
}

Token Lexer::NextToken() {
  SkipWhitespaceAndComments();
  if (AtEnd()) return MakeToken(TokenKind::END_OF_FILE, "", line_, column_);

  int line = line_;
  int column = column_;
  char c = Peek();

  if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') return LexIdentifierOrKeyword();
  if (std::isdigit(static_cast<unsigned char>(c))) return LexNumber();

  Advance();
  switch (c) {
    case '(': return MakeToken(TokenKind::LPAREN, "(", line, column);
    case ')': return MakeToken(TokenKind::RPAREN, ")", line, column);
    case '{': return MakeToken(TokenKind::LBRACE, "{", line, column);
    case '}': return MakeToken(TokenKind::RBRACE, "}", line, column);
    case '[': return MakeToken(TokenKind::LBRACKET, "[", line, column);
    case ']': return MakeToken(TokenKind::RBRACKET, "]", line, column);
    case ',': return MakeToken(TokenKind::COMMA, ",", line, column);
    case ':': return MakeToken(TokenKind::COLON, ":", line, column);
    case '+':
      if (Match('=')) return MakeToken(TokenKind::PLUS_ASSIGN, "+=", line, column);
      return MakeToken(TokenKind::PLUS, "+", line, column);
    case '-':
      if (Match('>')) return MakeToken(TokenKind::ARROW, "->", line, column);
      return MakeToken(TokenKind::MINUS, "-", line, column);
    case '*': return MakeToken(TokenKind::STAR, "*", line, column);
    case '/': return MakeToken(TokenKind::SLASH, "/", line, column);
    case '=': return MakeToken(TokenKind::ASSIGN, "=", line, column);
    case '.':
      if (Match('.')) return MakeToken(TokenKind::DOTDOT, "..", line, column);
      throw LexError("unexpected character '.' at line " + std::to_string(line) +
                      ", column " + std::to_string(column) +
                      ": a single '.' is not a valid token (did you mean '..'?)");
    default:
      throw LexError(std::string("unexpected character '") + c + "' at line " +
                      std::to_string(line) + ", column " + std::to_string(column));
  }
}

std::vector<Token> Lexer::Tokenize() {
  std::vector<Token> tokens;
  for (;;) {
    Token tok = NextToken();
    bool is_eof = tok.kind == TokenKind::END_OF_FILE;
    tokens.push_back(std::move(tok));
    if (is_eof) break;
  }
  return tokens;
}

}  // namespace parallix
