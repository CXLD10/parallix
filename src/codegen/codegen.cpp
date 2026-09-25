#include "parallix/codegen.h"

#include <sstream>
#include <unordered_map>

namespace parallix {

const char* CTypeName(TypeBase base) {
  switch (base) {
    case TypeBase::F32: return "float";
    case TypeBase::F64: return "double";
    case TypeBase::I32: return "int";
  }
  return "?";
}

namespace {

std::string Indent(int level) { return std::string(static_cast<size_t>(level) * 2, ' '); }

bool NeedsParens(const std::string& s) { return s.find(' ') != std::string::npos; }

std::string DimSizeExprC(const Dim& dim) {
  return dim.is_ident ? dim.ident : std::to_string(dim.int_value);
}

// Row-major flattening (specs/06: "A 2-D array A: f32[N, M] ... A[i, j]
// lowers to A[i * M + j]"), generalized to any dimensionality: dimension k's
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
      return ExprToC(*bin.lhs, arrays) + " " + ToString(bin.op) + " " + ExprToC(*bin.rhs, arrays);
    }
  }
  return "<?>";
}

// Identity element for a reduction's accumulator init. Only `+` is exercised
// by any fixture (T3); `*`/`min`/`max` are handled soundly but are a Phase 2
// simplification never exercised end-to-end (documented in PROGRESS.md).
std::string IdentityLiteral(ReduceOp op, TypeBase base) {
  bool is_int = (base == TypeBase::I32);
  switch (op) {
    case ReduceOp::ADD: return "0";
    case ReduceOp::MUL: return "1";
    case ReduceOp::MIN: return is_int ? "2147483647" : "1e30";
    case ReduceOp::MAX: return is_int ? "-2147483648" : "-1e30";
  }
  return "0";
}

struct EmitContext {
  bool parallel_variant;
  const Schedule* schedule;
  ReduceOp reduce_op;
  const std::string* accumulator;
  const std::unordered_map<std::string, ArrayParamInfo>* arrays;
};

void EmitStmtList(const std::vector<StmtPtr>& stmts, int indent, size_t& loop_counter,
                   const EmitContext& ctx, std::ostringstream& out);

void EmitStmt(const Stmt& stmt, int indent, size_t& loop_counter, const EmitContext& ctx,
              std::ostringstream& out) {
  switch (stmt.kind) {
    case StmtKind::LOOP: {
      const auto& loop = static_cast<const LoopStmt&>(stmt);
      size_t level_index = loop_counter++;
      bool attach_pragma = ctx.parallel_variant && ctx.schedule->kind == ScheduleKind::PARALLEL_OUTER &&
                            level_index == ctx.schedule->parallel_loop_index;
      if (attach_pragma) out << Indent(indent) << "#pragma omp parallel for\n";
      std::string lower_c = ExprToC(*loop.lower, *ctx.arrays);
      std::string upper_c = ExprToC(*loop.upper, *ctx.arrays);
      out << Indent(indent) << "for (int " << loop.index_var << " = " << lower_c << "; "
          << loop.index_var << " < " << upper_c << "; " << loop.index_var << "++) {\n";
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
        out << Indent(indent) << "#pragma omp parallel for reduction(" << ToString(ctx.reduce_op)
            << ":" << *ctx.accumulator << ")\n";
      }
      std::string lower_c = ExprToC(*red.lower, *ctx.arrays);
      std::string upper_c = ExprToC(*red.upper, *ctx.arrays);
      out << Indent(indent) << "for (int " << red.index_var << " = " << lower_c << "; "
          << red.index_var << " < " << upper_c << "; " << red.index_var << "++) {\n";
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
                            bool ret_is_array, bool ret_in_place) {
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
  result.return_is_array = ret_is_array;
  result.return_array_name = ret_is_array ? kernel.ret.name : "";
  result.return_c_type = ret_is_array ? "void" : CTypeName(kernel.ret.type.base);

  result.is_reduction = pir.is_reduction;
  result.reduce_op = pir.reduce_op;
  result.reduce_accumulator = pir.reduce_accumulator;

  // Dimension parameters: first-encountered order, params then return type
  // (specs/06 section 2).
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

  result.seq_function_name = kernel.name + "_seq";
  result.par_function_name =
      (schedule.kind == ScheduleKind::SEQUENTIAL_ONLY) ? "" : (kernel.name + "_par");

  std::ostringstream source;

  auto emit_function = [&](const std::string& func_name, bool parallel_variant) {
    std::string sig = BuildSignature(func_name, result.return_c_type, kernel, result.dim_params,
                                      ret_is_array, ret_in_place);
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

  result.c_source = source.str();
  return result;
}

}  // namespace parallix
