#pragma once

#include <memory>
#include <string>
#include <vector>

namespace parallix {

// ---- Types -----------------------------------------------------------

enum class TypeBase { F32, F64, I32 };

inline const char* ToString(TypeBase base) {
  switch (base) {
    case TypeBase::F32: return "f32";
    case TypeBase::F64: return "f64";
    case TypeBase::I32: return "i32";
  }
  return "unknown";
}

// A single array dimension: either a symbolic parameter (IDENT) or a
// compile-time-known constant (INT).
struct Dim {
  bool is_ident;
  std::string ident;       // valid if is_ident
  long long int_value = 0; // valid if !is_ident
};

// base ('[' dim (',' dim)* ']')? — dims empty means a scalar type.
struct TypeNode {
  TypeBase base;
  std::vector<Dim> dims;

  bool IsScalar() const { return dims.empty(); }
};

struct Param {
  std::string name;
  TypeNode type;
};

// ---- Expressions -------------------------------------------------------

enum class ExprKind { VAR_REF, INT_LITERAL, BINARY };

struct Expr {
  ExprKind kind;
  int line = 0;
  int column = 0;
  virtual ~Expr() = default;

 protected:
  explicit Expr(ExprKind k) : kind(k) {}
};
using ExprPtr = std::unique_ptr<Expr>;

// lvalue := IDENT ('[' expr (',' expr)* ']')?
// Also covers the bare-IDENT case of `factor` (scalar read).
struct VarRefExpr : Expr {
  std::string name;
  std::vector<ExprPtr> subscripts;  // empty => scalar reference

  VarRefExpr() : Expr(ExprKind::VAR_REF) {}
  bool IsScalar() const { return subscripts.empty(); }
};

struct IntLiteralExpr : Expr {
  long long value = 0;
  IntLiteralExpr() : Expr(ExprKind::INT_LITERAL) {}
};

enum class BinaryOp { ADD, SUB, MUL, DIV };

inline const char* ToString(BinaryOp op) {
  switch (op) {
    case BinaryOp::ADD: return "+";
    case BinaryOp::SUB: return "-";
    case BinaryOp::MUL: return "*";
    case BinaryOp::DIV: return "/";
  }
  return "?";
}

struct BinaryExpr : Expr {
  BinaryOp op;
  ExprPtr lhs;
  ExprPtr rhs;
  BinaryExpr() : Expr(ExprKind::BINARY) {}
};

// ---- Statements ----------------------------------------------------

enum class StmtKind { LOOP, REDUCTION, ASSIGN };

struct Stmt {
  StmtKind kind;
  int line = 0;
  int column = 0;
  virtual ~Stmt() = default;

 protected:
  explicit Stmt(StmtKind k) : kind(k) {}
};
using StmtPtr = std::unique_ptr<Stmt>;

enum class LoopMode { AUTO, PARALLEL, VECTORIZE, SEQ };

inline const char* ToString(LoopMode mode) {
  switch (mode) {
    case LoopMode::AUTO: return "auto";
    case LoopMode::PARALLEL: return "parallel";
    case LoopMode::VECTORIZE: return "vectorize";
    case LoopMode::SEQ: return "seq";
  }
  return "unknown";
}

// loop := mode IDENT 'in' expr '..' expr '{' stmt+ '}'
struct LoopStmt : Stmt {
  LoopMode mode;
  std::string index_var;
  ExprPtr lower;
  ExprPtr upper;
  std::vector<StmtPtr> body;
  LoopStmt() : Stmt(StmtKind::LOOP) {}
};

enum class ReduceOp { ADD, MUL, MIN, MAX };

inline const char* ToString(ReduceOp op) {
  switch (op) {
    case ReduceOp::ADD: return "+";
    case ReduceOp::MUL: return "*";
    case ReduceOp::MIN: return "min";
    case ReduceOp::MAX: return "max";
  }
  return "?";
}

// reduction := 'reduce' '(' op ')' IDENT 'in' expr '..' expr '{' stmt+ '}'
struct ReductionStmt : Stmt {
  ReduceOp op;
  std::string index_var;
  ExprPtr lower;
  ExprPtr upper;
  std::vector<StmtPtr> body;
  ReductionStmt() : Stmt(StmtKind::REDUCTION) {}
};

enum class AssignOp { SET, ADD_SET };  // '=' or '+='

inline const char* ToString(AssignOp op) {
  switch (op) {
    case AssignOp::SET: return "=";
    case AssignOp::ADD_SET: return "+=";
  }
  return "?";
}

// assign := lvalue ('=' | '+=') expr
struct AssignStmt : Stmt {
  std::unique_ptr<VarRefExpr> lvalue;
  AssignOp op;
  ExprPtr rhs;
  AssignStmt() : Stmt(StmtKind::ASSIGN) {}
};

// ---- Kernel / program -----------------------------------------------

// kernel := 'kernel' IDENT '(' params ')' '->' ret '{' stmt+ '}'
struct KernelDecl {
  std::string name;
  std::vector<Param> params;
  Param ret;
  std::vector<StmtPtr> body;
  int line = 0;
  int column = 0;
};

// program := kernel+
struct Program {
  std::vector<KernelDecl> kernels;
};

}  // namespace parallix
