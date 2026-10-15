#include "parallix/sema.h"

#include <sstream>

namespace parallix {

namespace {

std::string ExprToString(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return std::to_string(static_cast<const IntLiteralExpr&>(expr).value);
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      if (ref.subscripts.empty()) return ref.name;
      std::ostringstream out;
      out << ref.name << "[";
      for (size_t i = 0; i < ref.subscripts.size(); ++i) {
        if (i > 0) out << ", ";
        out << ExprToString(*ref.subscripts[i]);
      }
      out << "]";
      return out.str();
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      std::ostringstream out;
      out << ExprToString(*bin.lhs) << " " << ToString(bin.op) << " " << ExprToString(*bin.rhs);
      return out.str();
    }
    case ExprKind::CALL: {
      const auto& call = static_cast<const CallExpr&>(expr);
      std::ostringstream out;
      out << ToString(call.callee) << "(";
      for (size_t i = 0; i < call.args.size(); ++i) {
        if (i > 0) out << ", ";
        out << ExprToString(*call.args[i]);
      }
      out << ")";
      return out.str();
    }
  }
  return "<?>";
}

// The first comparison or built-in call found anywhere inside `expr`, or null.
// Those are value-producing constructs and must never reach an affine context.
const Expr* FindValueConstruct(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return nullptr;
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      for (const auto& sub : ref.subscripts) {
        if (const Expr* found = FindValueConstruct(*sub)) return found;
      }
      return nullptr;
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      if (IsComparison(bin.op)) return &expr;
      if (const Expr* found = FindValueConstruct(*bin.lhs)) return found;
      return FindValueConstruct(*bin.rhs);
    }
    case ExprKind::CALL:
      return &expr;
  }
  return nullptr;
}

// "COMPARISON", "SELECT", "MIN" or "MAX" -- the construct part of E_<X>_IN_<CONTEXT>.
std::string ConstructName(const Expr& expr) {
  if (expr.kind == ExprKind::CALL) {
    const char* name = ToString(static_cast<const CallExpr&>(expr).callee);
    std::string upper;
    for (const char* c = name; *c; ++c) upper += static_cast<char>(*c - 'a' + 'A');
    return upper;
  }
  return "COMPARISON";
}

struct AffineInfo {
  bool affine;
  bool depends_on_index;
};

AffineInfo AnalyzeAffine(const Expr& expr, const SymbolTable& table) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return {true, false};
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      if (!ref.subscripts.empty()) {
        // An array read nested inside a subscript/bound expression is never
        // affine: it is not a linear combination of indices and constants.
        return {false, false};
      }
      return {true, table.IsLoopIndex(ref.name)};
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      AffineInfo lhs = AnalyzeAffine(*bin.lhs, table);
      AffineInfo rhs = AnalyzeAffine(*bin.rhs, table);
      bool depends = lhs.depends_on_index || rhs.depends_on_index;
      bool affine;
      if (IsComparison(bin.op)) {
        affine = false;
      } else if (bin.op == BinaryOp::ADD || bin.op == BinaryOp::SUB) {
        affine = lhs.affine && rhs.affine;
      } else {
        // MUL or DIV: affine only if at least one side is index-independent.
        affine = lhs.affine && rhs.affine && !(lhs.depends_on_index && rhs.depends_on_index);
      }
      return {affine, depends};
    }
    case ExprKind::CALL:
      return {false, false};
  }
  return {false, false};
}

bool ExprReferencesName(const Expr& expr, const std::string& name) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return false;
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      if (ref.name == name) return true;
      for (const auto& sub : ref.subscripts) {
        if (ExprReferencesName(*sub, name)) return true;
      }
      return false;
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      return ExprReferencesName(*bin.lhs, name) || ExprReferencesName(*bin.rhs, name);
    }
    case ExprKind::CALL: {
      const auto& call = static_cast<const CallExpr&>(expr);
      for (const auto& arg : call.args) {
        if (ExprReferencesName(*arg, name)) return true;
      }
      return false;
    }
  }
  return false;
}

// `m = max(m, expr)` / `m = min(m, expr)` (either argument order) where expr
// does not itself read m.
bool IsMinMaxAccumulation(const AssignStmt& assign, ReduceOp op) {
  if (assign.op != AssignOp::SET || assign.rhs->kind != ExprKind::CALL) return false;
  const auto& call = static_cast<const CallExpr&>(*assign.rhs);
  CallKind want = op == ReduceOp::MIN ? CallKind::MIN : CallKind::MAX;
  if (call.callee != want) return false;
  const std::string& name = assign.lvalue->name;
  auto is_acc = [&](const Expr& e) {
    return e.kind == ExprKind::VAR_REF && static_cast<const VarRefExpr&>(e).IsScalar() &&
           static_cast<const VarRefExpr&>(e).name == name;
  };
  if (is_acc(*call.args[0])) return !ExprReferencesName(*call.args[1], name);
  if (is_acc(*call.args[1])) return !ExprReferencesName(*call.args[0], name);
  return false;
}

}  // namespace

void Sema::AnalyzeKernel(const KernelDecl& kernel, SymbolTable* table_out) {
  kernel_name_ = kernel.name;
  loop_depth_ = 0;
  SymbolTable table;
  for (const Param& param : kernel.params) {
    table.DeclareBinding(param, SymbolKind::PARAM);
  }
  table.DeclareBinding(kernel.ret, SymbolKind::RETURN);

  CheckStmtList(kernel.body, table, /*inside_reduction=*/false, ReduceOp::ADD);

  if (table_out != nullptr) *table_out = table;
}

void Sema::ResolveIdentifiers(const Expr& expr, const SymbolTable& table) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return;
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      if (table.Lookup(ref.name) == nullptr) {
        throw SemaError("[E_UNDECLARED_IDENTIFIER] undeclared identifier '" + ref.name +
                         "' referenced in kernel '" + kernel_name_ +
                         "': it is neither a parameter, a return binding, an array "
                         "dimension, nor an enclosing loop index");
      }
      for (const auto& sub : ref.subscripts) {
        ResolveIdentifiers(*sub, table);
      }
      return;
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      ResolveIdentifiers(*bin.lhs, table);
      ResolveIdentifiers(*bin.rhs, table);
      return;
    }
    case ExprKind::CALL: {
      for (const auto& arg : static_cast<const CallExpr&>(expr).args) {
        ResolveIdentifiers(*arg, table);
      }
      return;
    }
  }
}

void Sema::CheckShapes(const Expr& expr, const SymbolTable& table) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return;
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      const Symbol* sym = table.Lookup(ref.name);
      // Already validated by ResolveIdentifiers before CheckShapes runs.
      size_t declared_dims =
          (sym->kind == SymbolKind::PARAM || sym->kind == SymbolKind::RETURN)
              ? sym->type.dims.size()
              : 0;
      if (ref.subscripts.size() != declared_dims) {
        throw SemaError("[E_SHAPE_MISMATCH] shape mismatch in kernel '" + kernel_name_ + "': '" + ref.name +
                         "' is declared with " + std::to_string(declared_dims) +
                         " dimension(s) but referenced with " +
                         std::to_string(ref.subscripts.size()) +
                         " subscript(s) in `" + ExprToString(ref) + "`");
      }
      for (const auto& sub : ref.subscripts) {
        CheckShapes(*sub, table);
      }
      return;
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      CheckShapes(*bin.lhs, table);
      CheckShapes(*bin.rhs, table);
      return;
    }
    case ExprKind::CALL: {
      for (const auto& arg : static_cast<const CallExpr&>(expr).args) CheckShapes(*arg, table);
      return;
    }
  }
}

void Sema::CheckSubscriptsAffine(const Expr& expr, const SymbolTable& table) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return;
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      for (const auto& sub : ref.subscripts) {
        if (const Expr* v = FindValueConstruct(*sub)) {
          throw SemaError("[E_" + ConstructName(*v) + "_IN_SUBSCRIPT] `" + ExprToString(*v) +
                           "` is a value expression and is not allowed inside the subscript `" +
                           ExprToString(*sub) + "` of `" + ExprToString(ref) +
                           "`: subscripts must stay affine (a linear combination of loop "
                           "indices and constants) so dependence analysis remains exact");
        }
        AffineInfo info = AnalyzeAffine(*sub, table);
        if (!info.affine) {
          throw SemaError("[E_NON_AFFINE_SUBSCRIPT] non-affine subscript `" + ExprToString(*sub) +
                           "` in array reference `" + ExprToString(ref) +
                           "`: subscripts must be a linear combination of loop indices "
                           "and constants");
        }
        CheckSubscriptsAffine(*sub, table);
      }
      return;
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      CheckSubscriptsAffine(*bin.lhs, table);
      CheckSubscriptsAffine(*bin.rhs, table);
      return;
    }
    case ExprKind::CALL: {
      for (const auto& arg : static_cast<const CallExpr&>(expr).args) {
        CheckSubscriptsAffine(*arg, table);
      }
      return;
    }
  }
}

// select(cond, a, b): `cond` must itself be a comparison.
static void CheckCalls(const Expr& expr, const std::string& kernel_name) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return;
    case ExprKind::VAR_REF:
      for (const auto& sub : static_cast<const VarRefExpr&>(expr).subscripts) {
        CheckCalls(*sub, kernel_name);
      }
      return;
    case ExprKind::BINARY:
      CheckCalls(*static_cast<const BinaryExpr&>(expr).lhs, kernel_name);
      CheckCalls(*static_cast<const BinaryExpr&>(expr).rhs, kernel_name);
      return;
    case ExprKind::CALL: {
      const auto& call = static_cast<const CallExpr&>(expr);
      if (call.callee == CallKind::SELECT) {
        const Expr& cond = *call.args[0];
        bool is_cmp = cond.kind == ExprKind::BINARY &&
                      IsComparison(static_cast<const BinaryExpr&>(cond).op);
        if (!is_cmp) {
          throw SemaError("[E_SELECT_CONDITION] the condition of `" + ExprToString(call) +
                           "` in kernel '" + kernel_name +
                           "' must be a comparison (==, !=, <, <=, >, >=)");
        }
      }
      for (const auto& arg : call.args) CheckCalls(*arg, kernel_name);
      return;
    }
  }
}

void Sema::CheckBoundAffine(const Expr& bound, const SymbolTable& table,
                             const std::string& loop_var, const char* which) {
  if (const Expr* v = FindValueConstruct(bound)) {
    throw SemaError("[E_" + ConstructName(*v) + "_IN_BOUND] `" + ExprToString(*v) +
                     "` is a value expression and is not allowed in the " + which +
                     " bound `" + ExprToString(bound) + "` of loop index '" + loop_var +
                     "': loop bounds must stay affine");
  }
  AffineInfo info = AnalyzeAffine(bound, table);
  if (!info.affine) {
    throw SemaError("[E_NON_AFFINE_BOUND] non-affine " + std::string(which) + " bound `" + ExprToString(bound) +
                     "` for loop index '" + loop_var + "' in kernel '" + kernel_name_ +
                     "': loop bounds must be a linear combination of enclosing loop "
                     "indices and constants");
  }
  // A bound may itself contain array-ref subscripts in principle; validate them too.
  CheckSubscriptsAffine(bound, table);
}

void Sema::ValidateReductionAssign(const AssignStmt& assign, ReduceOp op) {
  if (!assign.lvalue->IsScalar()) return;  // only scalar accumulators are reduction-checked
  const std::string& name = assign.lvalue->name;
  // '+=' denotes accumulation by construction even though the rhs doesn't
  // textually repeat the lvalue's name (`s += A[i]` means `s = s + A[i]`).
  bool self_referential =
      assign.op == AssignOp::ADD_SET || ExprReferencesName(*assign.rhs, name);
  bool min_max = op == ReduceOp::MIN || op == ReduceOp::MAX;
  if (!self_referential && !min_max) {
    return;  // plain initialization (e.g. `s = 0`), not an accumulation step
  }

  bool valid = false;
  if (op == ReduceOp::ADD) {
    valid = assign.op == AssignOp::ADD_SET ||
            (assign.op == AssignOp::SET && assign.rhs->kind == ExprKind::BINARY &&
             static_cast<const BinaryExpr&>(*assign.rhs).op == BinaryOp::ADD);
  } else if (op == ReduceOp::MUL) {
    valid = assign.op == AssignOp::SET && assign.rhs->kind == ExprKind::BINARY &&
            static_cast<const BinaryExpr&>(*assign.rhs).op == BinaryOp::MUL;
  } else {
    valid = IsMinMaxAccumulation(assign, op);
  }

  if (!valid) {
    std::string hint = (op == ReduceOp::MIN || op == ReduceOp::MAX)
                           ? std::string(" (write `") + name + " = " + ToString(op) + "(" +
                                 name + ", <expr>)`)"
                           : std::string();
    throw SemaError("[E_REDUCTION_OPERATOR_MISMATCH] reduction operator mismatch in kernel '" +
                     kernel_name_ + "': '" + name + "' is combined with '" +
                     ToString(assign.op) + "' inside reduce(" + ToString(op) +
                     ") { }, but only the declared operator '" + ToString(op) +
                     "' is licensed there" + hint);
  }
}

void Sema::CheckStmtList(const std::vector<StmtPtr>& stmts, SymbolTable& table,
                          bool inside_reduction, ReduceOp enclosing_op) {
  for (const auto& stmt : stmts) {
    CheckStmt(*stmt, table, inside_reduction, enclosing_op);
  }
}

void Sema::CheckStmt(const Stmt& stmt, SymbolTable& table, bool inside_reduction,
                      ReduceOp enclosing_op) {
  switch (stmt.kind) {
    case StmtKind::LOOP: {
      const auto& loop = static_cast<const LoopStmt&>(stmt);
      ResolveIdentifiers(*loop.lower, table);
      ResolveIdentifiers(*loop.upper, table);
      CheckBoundAffine(*loop.lower, table, loop.index_var, "lower");
      CheckBoundAffine(*loop.upper, table, loop.index_var, "upper");

      table.PushLoopIndex(loop.index_var);
      loop_depth_++;
      CheckStmtList(loop.body, table, inside_reduction, enclosing_op);
      loop_depth_--;
      table.PopLoopIndex();
      return;
    }
    case StmtKind::REDUCTION: {
      const auto& red = static_cast<const ReductionStmt&>(stmt);
      if (loop_depth_ > 0) {
        throw SemaError("[E_REDUCTION_NOT_OUTERMOST] reduce(" + std::string(ToString(red.op)) +
                         ") over '" + red.index_var + "' in kernel '" + kernel_name_ +
                         "' is nested inside another loop; a reduction must be the "
                         "outermost loop of the kernel (put loops inside the reduction, "
                         "not around it)");
      }
      ResolveIdentifiers(*red.lower, table);
      ResolveIdentifiers(*red.upper, table);
      CheckBoundAffine(*red.lower, table, red.index_var, "lower");
      CheckBoundAffine(*red.upper, table, red.index_var, "upper");

      table.PushLoopIndex(red.index_var);
      loop_depth_++;
      CheckStmtList(red.body, table, /*inside_reduction=*/true, red.op);
      loop_depth_--;
      table.PopLoopIndex();
      return;
    }
    case StmtKind::ASSIGN: {
      const auto& assign = static_cast<const AssignStmt&>(stmt);
      ResolveIdentifiers(*assign.lvalue, table);
      ResolveIdentifiers(*assign.rhs, table);
      CheckShapes(*assign.lvalue, table);
      CheckShapes(*assign.rhs, table);
      CheckSubscriptsAffine(*assign.lvalue, table);
      CheckSubscriptsAffine(*assign.rhs, table);
      CheckCalls(*assign.lvalue, kernel_name_);
      CheckCalls(*assign.rhs, kernel_name_);
      const Symbol* target = table.Lookup(assign.lvalue->name);
      if (target->kind != SymbolKind::RETURN) {
        throw SemaError("[E_ASSIGN_TARGET] '" + assign.lvalue->name + "' in kernel '" +
                         kernel_name_ +
                         "' is not the kernel's return binding; a kernel may only write to "
                         "the value named after '->' (inputs, dimensions and loop indices "
                         "are read-only)");
      }
      if (assign.lvalue->IsScalar() && !inside_reduction) {
        throw SemaError("[E_SCALAR_WRITE_OUTSIDE_REDUCTION] scalar '" + assign.lvalue->name +
                         "' is written in kernel '" + kernel_name_ +
                         "' outside a reduce(...) loop; every iteration would write the same "
                         "location, which is not parallelizable. Use reduce(op) to accumulate");
      }
      if (inside_reduction) {
        ValidateReductionAssign(assign, enclosing_op);
      }
      return;
    }
  }
}

}  // namespace parallix
