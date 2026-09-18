#pragma once

#include <stdexcept>
#include <string>

#include "parallix/ast.h"
#include "parallix/symbol_table.h"

namespace parallix {

// Thrown on the first semantic violation found (specs/02's four rules:
// affine-boundedness, shape checking, reduction validation, symbol
// resolution). Message names the offending construct and the violated rule
// per constitution rule 5 — never a bare "semantic error".
class SemaError : public std::runtime_error {
 public:
  explicit SemaError(const std::string& message) : std::runtime_error(message) {}
};

class Sema {
 public:
  // Analyzes one kernel and throws SemaError on the first violation found.
  // On success, if `table_out` is non-null, it receives the kernel's
  // resolved symbol table (params/return/dims) for reuse by lowering (M4).
  void AnalyzeKernel(const KernelDecl& kernel, SymbolTable* table_out = nullptr);

 private:
  std::string kernel_name_;

  void ResolveIdentifiers(const Expr& expr, const SymbolTable& table);
  void CheckShapes(const Expr& expr, const SymbolTable& table);
  void CheckSubscriptsAffine(const Expr& expr, const SymbolTable& table);
  void CheckBoundAffine(const Expr& bound, const SymbolTable& table, const std::string& loop_var,
                         const char* which);

  void CheckStmt(const Stmt& stmt, SymbolTable& table, bool inside_reduction,
                  ReduceOp enclosing_op);
  void CheckStmtList(const std::vector<StmtPtr>& stmts, SymbolTable& table,
                      bool inside_reduction, ReduceOp enclosing_op);
  void ValidateReductionAssign(const AssignStmt& assign, ReduceOp op);
};

}  // namespace parallix
