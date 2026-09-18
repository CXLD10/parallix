#include "parallix/lexer.h"

#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

using parallix::Lexer;
using parallix::Token;
using parallix::TokenKind;

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct Expected {
  TokenKind kind;
  std::string lexeme;
};

void ExpectTokens(const std::vector<Token>& tokens, const std::vector<Expected>& expected) {
  ASSERT_EQ(tokens.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(tokens[i].kind, expected[i].kind)
        << "at index " << i << ": got " << parallix::ToString(tokens[i].kind) << " expected "
        << parallix::ToString(expected[i].kind);
    EXPECT_EQ(tokens[i].lexeme, expected[i].lexeme) << "at index " << i;
  }
}

}  // namespace

TEST(Lexer, T1VectorAdd) {
  std::string src = ReadFile("examples/t1_vector_add.plx");
  ASSERT_FALSE(src.empty());
  Lexer lexer(src);
  std::vector<Token> tokens = lexer.Tokenize();

  std::vector<Expected> expected = {
      {TokenKind::KERNEL, "kernel"},
      {TokenKind::IDENT, "vector_add"},
      {TokenKind::LPAREN, "("},
      {TokenKind::IDENT, "A"},
      {TokenKind::COLON, ":"},
      {TokenKind::F32, "f32"},
      {TokenKind::LBRACKET, "["},
      {TokenKind::IDENT, "N"},
      {TokenKind::RBRACKET, "]"},
      {TokenKind::COMMA, ","},
      {TokenKind::IDENT, "B"},
      {TokenKind::COLON, ":"},
      {TokenKind::F32, "f32"},
      {TokenKind::LBRACKET, "["},
      {TokenKind::IDENT, "N"},
      {TokenKind::RBRACKET, "]"},
      {TokenKind::RPAREN, ")"},
      {TokenKind::ARROW, "->"},
      {TokenKind::IDENT, "C"},
      {TokenKind::COLON, ":"},
      {TokenKind::F32, "f32"},
      {TokenKind::LBRACKET, "["},
      {TokenKind::IDENT, "N"},
      {TokenKind::RBRACKET, "]"},
      {TokenKind::LBRACE, "{"},
      {TokenKind::AUTO, "auto"},
      {TokenKind::IDENT, "i"},
      {TokenKind::IN, "in"},
      {TokenKind::INT_LITERAL, "0"},
      {TokenKind::DOTDOT, ".."},
      {TokenKind::IDENT, "N"},
      {TokenKind::LBRACE, "{"},
      {TokenKind::IDENT, "C"},
      {TokenKind::LBRACKET, "["},
      {TokenKind::IDENT, "i"},
      {TokenKind::RBRACKET, "]"},
      {TokenKind::ASSIGN, "="},
      {TokenKind::IDENT, "A"},
      {TokenKind::LBRACKET, "["},
      {TokenKind::IDENT, "i"},
      {TokenKind::RBRACKET, "]"},
      {TokenKind::PLUS, "+"},
      {TokenKind::IDENT, "B"},
      {TokenKind::LBRACKET, "["},
      {TokenKind::IDENT, "i"},
      {TokenKind::RBRACKET, "]"},
      {TokenKind::RBRACE, "}"},
      {TokenKind::RBRACE, "}"},
      {TokenKind::END_OF_FILE, ""},
  };

  ExpectTokens(tokens, expected);
}

TEST(Lexer, T2PrefixLike) {
  std::string src = ReadFile("examples/t2_prefix_like.plx");
  ASSERT_FALSE(src.empty());
  Lexer lexer(src);
  std::vector<Token> tokens = lexer.Tokenize();

  std::vector<Expected> expected = {
      {TokenKind::KERNEL, "kernel"},
      {TokenKind::IDENT, "prefix_like"},
      {TokenKind::LPAREN, "("},
      {TokenKind::IDENT, "A"},
      {TokenKind::COLON, ":"},
      {TokenKind::F32, "f32"},
      {TokenKind::LBRACKET, "["},
      {TokenKind::IDENT, "N"},
      {TokenKind::RBRACKET, "]"},
      {TokenKind::COMMA, ","},
      {TokenKind::IDENT, "B"},
      {TokenKind::COLON, ":"},
      {TokenKind::F32, "f32"},
      {TokenKind::LBRACKET, "["},
      {TokenKind::IDENT, "N"},
      {TokenKind::RBRACKET, "]"},
      {TokenKind::RPAREN, ")"},
      {TokenKind::ARROW, "->"},
      {TokenKind::IDENT, "A"},
      {TokenKind::COLON, ":"},
      {TokenKind::F32, "f32"},
      {TokenKind::LBRACKET, "["},
      {TokenKind::IDENT, "N"},
      {TokenKind::RBRACKET, "]"},
      {TokenKind::LBRACE, "{"},
      {TokenKind::AUTO, "auto"},
      {TokenKind::IDENT, "i"},
      {TokenKind::IN, "in"},
      {TokenKind::INT_LITERAL, "1"},
      {TokenKind::DOTDOT, ".."},
      {TokenKind::IDENT, "N"},
      {TokenKind::LBRACE, "{"},
      {TokenKind::IDENT, "A"},
      {TokenKind::LBRACKET, "["},
      {TokenKind::IDENT, "i"},
      {TokenKind::RBRACKET, "]"},
      {TokenKind::ASSIGN, "="},
      {TokenKind::IDENT, "A"},
      {TokenKind::LBRACKET, "["},
      {TokenKind::IDENT, "i"},
      {TokenKind::MINUS, "-"},
      {TokenKind::INT_LITERAL, "1"},
      {TokenKind::RBRACKET, "]"},
      {TokenKind::PLUS, "+"},
      {TokenKind::IDENT, "B"},
      {TokenKind::LBRACKET, "["},
      {TokenKind::IDENT, "i"},
      {TokenKind::RBRACKET, "]"},
      {TokenKind::RBRACE, "}"},
      {TokenKind::RBRACE, "}"},
      {TokenKind::END_OF_FILE, ""},
  };

  ExpectTokens(tokens, expected);
}

TEST(Lexer, DiscardsLineComments) {
  std::string src = "// this is a comment\nkernel // trailing comment\nfoo";
  Lexer lexer(src);
  std::vector<Token> tokens = lexer.Tokenize();

  std::vector<Expected> expected = {
      {TokenKind::KERNEL, "kernel"},
      {TokenKind::IDENT, "foo"},
      {TokenKind::END_OF_FILE, ""},
  };
  ExpectTokens(tokens, expected);
}
