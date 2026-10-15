#include "parallix/codegen.h"

#include <sstream>
#include <unordered_map>

namespace parallix {

const char* CTypeName(TypeBase base) {
  switch (base) {
    case TypeBase::F32: return "float";
    case TypeBase::F64: return "double";
    case TypeBase::I32: return "int";
    case TypeBase::I64: return "long long";
  }
  return "?";
}

namespace {

std::string Indent(int level) { return std::string(static_cast<size_t>(level) * 2, ' '); }

bool NeedsParens(const std::string& s) { return s.find(' ') != std::string::npos; }

std::string DimSizeExprC(const Dim& dim) {
  return dim.is_ident ? dim.ident : std::to_string(dim.int_value);
}

// Row-major flattening, generalized to any dimensionality: dimension k's
// coefficient is the product of every later dimension's size.
std::string BuildRowMajorOffset(const std::vector<Dim>& dims,
                                 const std::vector<std::string>& subscripts_c) {
  size_t n = dims.size();
  if (n == 1) return subscripts_c[0];
  std::vector<std::string> terms;
  for (size_t k = 0; k < n; ++k) {
    std::string coeff;
    for (size_t m = k + 1; m < n; ++m) {
      if (!coeff.empty()) coeff += " * ";
      coeff += DimSizeExprC(dims[m]);
    }
    std::string term = NeedsParens(subscripts_c[k]) ? ("(" + subscripts_c[k] + ")") : subscripts_c[k];
    if (!coeff.empty()) term += " * " + coeff;
    terms.push_back(term);
  }
  std::string result;
  for (size_t i = 0; i < terms.size(); ++i) {
    if (i > 0) result += " + ";
    result += terms[i];
  }
  return result;
}

// Operator precedence of the C text an expression renders to. Atoms
// (references, literals, calls, parenthesized comparisons) bind tightest.
int CPrecedence(const Expr& expr) {
  if (expr.kind != ExprKind::BINARY) return 9;
  switch (static_cast<const BinaryExpr&>(expr).op) {
    case BinaryOp::ADD: case BinaryOp::SUB: return 1;
    case BinaryOp::MUL: case BinaryOp::DIV: return 2;
    default: return 9;  // comparisons are always rendered fully parenthesized
  }
}

std::string ExprToC(const Expr& expr, const std::unordered_map<std::string, ArrayParamInfo>& arrays) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL:
      return std::to_string(static_cast<const IntLiteralExpr&>(expr).value);
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(expr);
      if (ref.subscripts.empty()) return ref.name;
      std::vector<std::string> subs_c;
      for (const auto& sub : ref.subscripts) subs_c.push_back(ExprToC(*sub, arrays));
      auto it = arrays.find(ref.name);
      const std::vector<Dim>& dims = it->second.dims;
      return ref.name + "[" + BuildRowMajorOffset(dims, subs_c) + "]";
    }
    case ExprKind::BINARY: {
      const auto& bin = static_cast<const BinaryExpr&>(expr);
      std::string l = ExprToC(*bin.lhs, arrays);
      std::string r = ExprToC(*bin.rhs, arrays);
      if (IsComparison(bin.op)) {
        return "(" + l + " " + ToString(bin.op) + " " + r + ")";
      }
      int p = CPrecedence(expr);
      if (CPrecedence(*bin.lhs) < p) l = "(" + l + ")";
      if (CPrecedence(*bin.rhs) <= p) r = "(" + r + ")";
      return l + " " + ToString(bin.op) + " " + r;
    }
    case ExprKind::CALL: {
      const auto& call = static_cast<const CallExpr&>(expr);
      std::vector<std::string> args;
      for (const auto& arg : call.args) args.push_back(ExprToC(*arg, arrays));
      if (call.callee == CallKind::SELECT) {
        return "(" + args[0] + " ? " + args[1] + " : " + args[2] + ")";
      }
      return std::string(call.callee == CallKind::MIN ? "PLX_MIN(" : "PLX_MAX(") + args[0] +
             ", " + args[1] + ")";
    }
  }
  return "<?>";
}

bool UsesMinMax(const std::vector<StmtPtr>& stmts);

bool ExprUsesMinMax(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::INT_LITERAL: return false;
    case ExprKind::VAR_REF:
      for (const auto& sub : static_cast<const VarRefExpr&>(expr).subscripts) {
        if (ExprUsesMinMax(*sub)) return true;
      }
      return false;
    case ExprKind::BINARY:
      return ExprUsesMinMax(*static_cast<const BinaryExpr&>(expr).lhs) ||
             ExprUsesMinMax(*static_cast<const BinaryExpr&>(expr).rhs);
    case ExprKind::CALL: {
      const auto& call = static_cast<const CallExpr&>(expr);
      if (call.callee != CallKind::SELECT) return true;
      for (const auto& arg : call.args) {
        if (ExprUsesMinMax(*arg)) return true;
      }
      return false;
    }
  }
  return false;
}

bool UsesMinMax(const std::vector<StmtPtr>& stmts) {
  for (const auto& stmt : stmts) {
    switch (stmt->kind) {
      case StmtKind::LOOP:
        if (UsesMinMax(static_cast<const LoopStmt&>(*stmt).body)) return true;
        break;
      case StmtKind::REDUCTION:
        if (UsesMinMax(static_cast<const ReductionStmt&>(*stmt).body)) return true;
        break;
      case StmtKind::ASSIGN:
        if (ExprUsesMinMax(*static_cast<const AssignStmt&>(*stmt).rhs)) return true;
        break;
    }
  }
  return false;
}

// Identity element for a reduction's accumulator init. Float min/max use the
// largest finite value of the type (data are assumed finite).
std::string IdentityLiteral(ReduceOp op, TypeBase base) {
  switch (op) {
    case ReduceOp::ADD: return "0";
    case ReduceOp::MUL: return "1";
    case ReduceOp::MIN:
      switch (base) {
        case TypeBase::I32: return "2147483647";
        case TypeBase::I64: return "9223372036854775807LL";
        case TypeBase::F32: return "3.4028234663852886e+38f";
        case TypeBase::F64: return "1.7976931348623157e+308";
      }
      break;
    case ReduceOp::MAX:
      switch (base) {
        case TypeBase::I32: return "(-2147483647 - 1)";
        case TypeBase::I64: return "(-9223372036854775807LL - 1)";
        case TypeBase::F32: return "-3.4028234663852886e+38f";
        case TypeBase::F64: return "-1.7976931348623157e+308";
      }
      break;
  }
  return "0";
}

struct EmitContext {
  bool parallel_variant;
  bool ord_variant = false;
  const Schedule* schedule;
  ReduceOp reduce_op;
  const std::string* accumulator;
  const std::unordered_map<std::string, ArrayParamInfo>* arrays;
};

void EmitStmtList(const std::vector<StmtPtr>& stmts, int indent, size_t& loop_counter,
                   const EmitContext& ctx, std::ostringstream& out);

// The `for` header. In the order-permuted variant the outermost loop walks a
// caller-supplied permutation of its iterations instead of lower..upper.
void EmitLoopHeader(const std::string& var, const std::string& lower_c,
                    const std::string& upper_c, size_t level_index, int indent,
                    const EmitContext& ctx, std::ostringstream& out) {
  if (ctx.ord_variant && level_index == 0) {
    out << Indent(indent) << "for (int plx_k = 0; plx_k < (" << upper_c << ") - (" << lower_c
        << "); plx_k++) {\n"
        << Indent(indent + 1) << "int " << var << " = (" << lower_c << ") + plx_order[plx_k];\n";
    return;
  }
  out << Indent(indent) << "for (int " << var << " = " << lower_c << "; " << var << " < "
      << upper_c << "; " << var << "++) {\n";
}

void EmitStmt(const Stmt& stmt, int indent, size_t& loop_counter, const EmitContext& ctx,
              std::ostringstream& out) {
  switch (stmt.kind) {
    case StmtKind::LOOP: {
      const auto& loop = static_cast<const LoopStmt&>(stmt);
      size_t level_index = loop_counter++;
      bool attach_pragma = ctx.parallel_variant && ctx.schedule->kind == ScheduleKind::PARALLEL_OUTER &&
                            level_index == ctx.schedule->parallel_loop_index;
      if (attach_pragma) {
        out << Indent(indent) << "#pragma omp parallel for";
        if (!ctx.schedule->omp_schedule.empty()) out << " schedule(" << ctx.schedule->omp_schedule << ")";
        out << "\n";
      }
      std::string lower_c = ExprToC(*loop.lower, *ctx.arrays);
      std::string upper_c = ExprToC(*loop.upper, *ctx.arrays);
      EmitLoopHeader(loop.index_var, lower_c, upper_c, level_index, indent, ctx, out);
      EmitStmtList(loop.body, indent + 1, loop_counter, ctx, out);
      out << Indent(indent) << "}\n";
      return;
    }
    case StmtKind::REDUCTION: {
      const auto& red = static_cast<const ReductionStmt&>(stmt);
      size_t level_index = loop_counter++;
      bool attach_pragma = ctx.parallel_variant &&
                            ctx.schedule->kind == ScheduleKind::PARALLEL_REDUCTION &&
                            level_index == ctx.schedule->parallel_loop_index;
      if (attach_pragma) {
        out << Indent(indent) << "#pragma omp parallel for";
        if (!ctx.schedule->omp_schedule.empty()) out << " schedule(" << ctx.schedule->omp_schedule << ")";
        out << " reduction(" << ToString(ctx.reduce_op) << ":" << *ctx.accumulator << ")\n";
      }
      std::string lower_c = ExprToC(*red.lower, *ctx.arrays);
      std::string upper_c = ExprToC(*red.upper, *ctx.arrays);
      EmitLoopHeader(red.index_var, lower_c, upper_c, level_index, indent, ctx, out);
      EmitStmtList(red.body, indent + 1, loop_counter, ctx, out);
      out << Indent(indent) << "}\n";
      return;
    }
    case StmtKind::ASSIGN: {
      const auto& assign = static_cast<const AssignStmt&>(stmt);
      std::string lvalue_c = ExprToC(*assign.lvalue, *ctx.arrays);
      std::string rhs_c = ExprToC(*assign.rhs, *ctx.arrays);
      out << Indent(indent) << lvalue_c << " " << ToString(assign.op) << " " << rhs_c << ";\n";
      return;
    }
  }
}

void EmitStmtList(const std::vector<StmtPtr>& stmts, int indent, size_t& loop_counter,
                   const EmitContext& ctx, std::ostringstream& out) {
  for (const auto& stmt : stmts) EmitStmt(*stmt, indent, loop_counter, ctx, out);
}

std::string BuildSignature(const std::string& func_name, const std::string& return_c_type,
                            const KernelDecl& kernel, const std::vector<std::string>& dim_params,
                            bool ret_is_array, bool ret_in_place, bool with_order = false) {
  std::ostringstream sig;
  sig << return_c_type << " " << func_name << "(";
  bool first = true;
  auto add = [&](const std::string& text) {
    if (!first) sig << ", ";
    first = false;
    sig << text;
  };
  for (const Param& p : kernel.params) {
    if (p.type.IsScalar()) {
      add(std::string(CTypeName(p.type.base)) + " " + p.name);
    } else {
      bool is_in_place_output = ret_is_array && ret_in_place && kernel.ret.name == p.name;
      std::string qualifier = is_in_place_output ? "" : "const ";
      add(qualifier + std::string(CTypeName(p.type.base)) + "* " + p.name);
    }
  }
  if (ret_is_array && !ret_in_place) {
    add(std::string(CTypeName(kernel.ret.type.base)) + "* " + kernel.ret.name);
  }
  for (const std::string& dim : dim_params) {
    add("int " + dim);
  }
  if (with_order) add("const int* plx_order");
  sig << ")";
  return sig.str();
}

}  // namespace

CodegenResult GenerateCCode(const KernelDecl& kernel, const PIRKernel& pir,
                            const Schedule& schedule) {
  CodegenResult result;
  result.kernel_name = kernel.name;

  bool ret_is_array = !kernel.ret.type.IsScalar();
  bool ret_in_place = false;
  if (ret_is_array) {
    for (const Param& p : kernel.params) {
      if (p.name == kernel.ret.name) {
        ret_in_place = true;
        break;
      }
    }
  }
  result.return_base = kernel.ret.type.base;
  result.return_is_array = ret_is_array;
  result.return_array_name = ret_is_array ? kernel.ret.name : "";
  result.return_c_type = ret_is_array ? "void" : CTypeName(kernel.ret.type.base);

  result.is_reduction = pir.is_reduction;
  result.reduce_op = pir.reduce_op;
  result.reduce_accumulator = pir.reduce_accumulator;

  // Dimension parameters: first-encountered order, params then return type
  //.
  std::vector<std::string> seen_dims;
  auto collect_dims = [&](const TypeNode& type) {
    for (const Dim& d : type.dims) {
      if (!d.is_ident) continue;
      bool already = false;
      for (const auto& s : seen_dims) {
        if (s == d.ident) { already = true; break; }
      }
      if (!already) seen_dims.push_back(d.ident);
    }
  };
  for (const Param& p : kernel.params) collect_dims(p.type);
  collect_dims(kernel.ret.type);
  result.dim_params = seen_dims;

  // Array metadata (for the harness generator and for subscript codegen).
  std::unordered_map<std::string, ArrayParamInfo> arrays_map;
  for (const Param& p : kernel.params) {
    if (p.type.IsScalar()) continue;
    ArrayParamInfo info;
    info.name = p.name;
    info.elem_type = p.type.base;
    info.dims = p.type.dims;
    info.is_input = true;
    info.is_output = ret_is_array && ret_in_place && kernel.ret.name == p.name;
    result.arrays.push_back(info);
    arrays_map[info.name] = info;
  }
  if (ret_is_array && !ret_in_place) {
    ArrayParamInfo info;
    info.name = kernel.ret.name;
    info.elem_type = kernel.ret.type.base;
    info.dims = kernel.ret.type.dims;
    info.is_input = false;
    info.is_output = true;
    result.arrays.push_back(info);
    arrays_map[info.name] = info;
  }

  for (const Param& p : kernel.params) {
    if (p.type.IsScalar()) {
      result.scalars.push_back({p.name, p.type.base});
      result.arg_order.push_back({p.name, ArgSlot::SCALAR});
    } else {
      result.arg_order.push_back({p.name, ArgSlot::ARRAY});
    }
  }
  if (ret_is_array && !ret_in_place) result.arg_order.push_back({kernel.ret.name, ArgSlot::ARRAY});
  for (const std::string& dim : seen_dims) result.arg_order.push_back({dim, ArgSlot::DIM});

  {
    // Outermost loop trip count, as a C expression over dim params.
    const Expr* lower = nullptr;
    const Expr* upper = nullptr;
    if (!kernel.body.empty() && kernel.body[0]->kind == StmtKind::LOOP) {
      lower = static_cast<const LoopStmt&>(*kernel.body[0]).lower.get();
      upper = static_cast<const LoopStmt&>(*kernel.body[0]).upper.get();
    } else if (!kernel.body.empty() && kernel.body[0]->kind == StmtKind::REDUCTION) {
      lower = static_cast<const ReductionStmt&>(*kernel.body[0]).lower.get();
      upper = static_cast<const ReductionStmt&>(*kernel.body[0]).upper.get();
    }
    if (lower && upper) {
      result.trip_count_c = "(" + ExprToC(*upper, arrays_map) + ") - (" + ExprToC(*lower, arrays_map) + ")";
    }
  }

  result.seq_function_name = kernel.name + "_seq";
  result.ord_function_name = kernel.name + "_ord";
  result.par_function_name =
      (schedule.kind == ScheduleKind::SEQUENTIAL_ONLY) ? "" : (kernel.name + "_par");

  std::ostringstream source;
  if (UsesMinMax(kernel.body)) {
    source << "#define PLX_MIN(a, b) (((a) < (b)) ? (a) : (b))\n"
              "#define PLX_MAX(a, b) (((a) > (b)) ? (a) : (b))\n\n";
  }

  auto emit_function = [&](const std::string& func_name, bool parallel_variant,
                           bool ord_variant = false) {
    std::string sig = BuildSignature(func_name, result.return_c_type, kernel, result.dim_params,
                                      ret_is_array, ret_in_place, ord_variant);
    source << sig << " {\n";
    if (pir.is_reduction) {
      source << "  " << result.return_c_type << " " << pir.reduce_accumulator << " = "
             << IdentityLiteral(pir.reduce_op, kernel.ret.type.base) << ";\n";
    } else if (!ret_is_array) {
      // See the non-reduction scalar return note below: never exercised by
      // any fixture, but must still be a valid declaration to compile.
      source << "  " << result.return_c_type << " " << kernel.ret.name << " = 0;\n";
    }
    EmitContext ctx;
    ctx.parallel_variant = parallel_variant;
    ctx.ord_variant = ord_variant;
    ctx.schedule = &schedule;
    ctx.reduce_op = pir.reduce_op;
    ctx.accumulator = &pir.reduce_accumulator;
    ctx.arrays = &arrays_map;
    size_t loop_counter = 0;
    EmitStmtList(kernel.body, 1, loop_counter, ctx, source);
    if (pir.is_reduction) {
      source << "  return " << pir.reduce_accumulator << ";\n";
    } else if (!ret_is_array) {
      // Non-reduction scalar return: not exercised by any fixture (all six
      // kernels either return an array or are a reduction), but handled
      // soundly rather than left to crash — the DSL's only scalar-producing
      // construct today is a reduction.
      source << "  return " << kernel.ret.name << ";\n";
    }
    source << "}\n";
  };

  emit_function(result.seq_function_name, /*parallel_variant=*/false);
  if (!result.par_function_name.empty()) {
    source << "\n";
    emit_function(result.par_function_name, /*parallel_variant=*/true);
  }
  source << "\n";
  emit_function(result.ord_function_name, /*parallel_variant=*/false, /*ord_variant=*/true);

  result.c_source = source.str();
  return result;
}

}  // namespace parallix
