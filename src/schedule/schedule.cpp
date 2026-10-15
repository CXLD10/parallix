#include "parallix/schedule.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace parallix {

Schedule GenerateSchedule(const PIRKernel& kernel, const DependenceReport& report) {
  Schedule schedule;
  switch (report.verdict) {
    case Verdict::UNSAFE:
      schedule.kind = ScheduleKind::SEQUENTIAL_ONLY;
      schedule.reason =
          "UNSAFE verdict: there is nothing safe to parallelize and nothing to "
          "corroborate, so no parallel variant is generated for this kernel";
      break;
    case Verdict::SAFE_AS_REDUCTION:
      schedule.kind = ScheduleKind::PARALLEL_REDUCTION;
      schedule.parallel_loop_index = 0;
      schedule.reason = "SAFE_AS_REDUCTION verdict: parallelize the reduction loop '" +
                         kernel.loop_nest.front().index_var +
                         "' with an OpenMP reduction(" + ToString(kernel.reduce_op) + ":" +
                         kernel.reduce_accumulator + ") clause";
      break;
    case Verdict::SAFE:
    default:
      schedule.kind = ScheduleKind::PARALLEL_OUTER;
      schedule.parallel_loop_index = 0;
      schedule.reason = "SAFE verdict: parallelize the outermost loop '" +
                         kernel.loop_nest.front().index_var + "' with OpenMP";
      break;
  }
  if (schedule.kind != ScheduleKind::SEQUENTIAL_ONLY) {
    const std::string& outer = kernel.loop_nest.front().index_var;
    for (size_t l = 1; l < kernel.loop_nest.size(); ++l) {
      for (const AffineExpr* e : {&kernel.loop_nest[l].lower, &kernel.loop_nest[l].upper}) {
        for (const auto& term : e->terms) {
          if (term.first == outer) schedule.omp_schedule = "dynamic, 4";
        }
      }
    }
  }
  return schedule;
}

}  // namespace parallix

namespace parallix {

namespace {

using Env = std::map<std::string, long long>;

long long EvalAffine(const AffineExpr& e, const Env& env) {
  long long v = e.constant;
  for (const auto& [name, coeff] : e.terms) {
    auto it = env.find(name);
    if (it != env.end()) v += coeff * it->second;
  }
  return v;
}

// Number of body executions in the nest from `level` inward. Levels with more
// than kMaxExact iterations are sampled at kMaxExact evenly spaced points and
// scaled (exact for rectangular nests, a close estimate for triangular ones).
// ponytail: sampling keeps this O(4096^(depth-1)); exact closed forms if needed.
double CountIterations(const PIRKernel& k, size_t level, Env& env) {
  const LoopLevel& lv = k.loop_nest[level];
  long long lo = EvalAffine(lv.lower, env), hi = EvalAffine(lv.upper, env);
  if (hi <= lo) return 0;
  if (level + 1 == k.loop_nest.size()) return static_cast<double>(hi - lo);
  const long long kMaxExact = 4096;
  long long trip = hi - lo;
  long long samples = std::min(trip, kMaxExact);
  double total = 0;
  for (long long s = 0; s < samples; ++s) {
    long long v = lo + (samples == trip ? s : (s * trip) / samples);
    env[lv.index_var] = v;
    total += CountIterations(k, level + 1, env);
  }
  env.erase(lv.index_var);
  return total * (static_cast<double>(trip) / static_cast<double>(samples));
}

struct BodyCost {
  double cycles = 0;
  double refs = 0;
};

bool Mentions(const Expr& e, const std::string& name) {
  switch (e.kind) {
    case ExprKind::INT_LITERAL: return false;
    case ExprKind::VAR_REF: {
      const auto& r = static_cast<const VarRefExpr&>(e);
      if (r.name == name) return true;
      for (const auto& sub : r.subscripts) if (Mentions(*sub, name)) return true;
      return false;
    }
    case ExprKind::BINARY:
      return Mentions(*static_cast<const BinaryExpr&>(e).lhs, name) || Mentions(*static_cast<const BinaryExpr&>(e).rhs, name);
    case ExprKind::CALL:
      for (const auto& a : static_cast<const CallExpr&>(e).args) if (Mentions(*a, name)) return true;
      return false;
  }
  return false;
}

// An array reference whose subscripts do not involve the innermost loop index
// is loop-invariant there, so a compiler hoists it out of the loop: it costs
// nothing per body execution.
bool Varies(const VarRefExpr& ref, const std::string& innermost) {
  for (const auto& sub : ref.subscripts) if (Mentions(*sub, innermost)) return true;
  return false;
}

void CostExpr(const Expr& e, const CostModelParams& p, const std::string& innermost, BodyCost& c) {
  switch (e.kind) {
    case ExprKind::INT_LITERAL: return;
    case ExprKind::VAR_REF: {
      const auto& ref = static_cast<const VarRefExpr&>(e);
      if (!ref.subscripts.empty() && Varies(ref, innermost)) {
        c.cycles += p.cycles_per_ref;
        c.refs += 1;
      }
      return;
    }
    case ExprKind::BINARY: {
      const auto& b = static_cast<const BinaryExpr&>(e);
      c.cycles += b.op == BinaryOp::DIV ? p.cycles_per_div : p.cycles_per_op;
      CostExpr(*b.lhs, p, innermost, c);
      CostExpr(*b.rhs, p, innermost, c);
      return;
    }
    case ExprKind::CALL:
      c.cycles += p.cycles_per_op;
      for (const auto& a : static_cast<const CallExpr&>(e).args) CostExpr(*a, p, innermost, c);
      return;
  }
}

void CostStmts(const std::vector<StmtPtr>& stmts, const CostModelParams& p, const std::string& innermost, BodyCost& c) {
  for (const auto& s : stmts) {
    switch (s->kind) {
      case StmtKind::LOOP: CostStmts(static_cast<const LoopStmt&>(*s).body, p, innermost, c); break;
      case StmtKind::REDUCTION: CostStmts(static_cast<const ReductionStmt&>(*s).body, p, innermost, c); break;
      case StmtKind::ASSIGN: {
        const auto& a = static_cast<const AssignStmt&>(*s);
        if (!a.lvalue->subscripts.empty() && Varies(*a.lvalue, innermost)) {
          c.cycles += p.cycles_per_ref;
          c.refs += 1;
        }
        if (a.op == AssignOp::ADD_SET) c.cycles += p.cycles_per_op;
        CostExpr(*a.rhs, p, innermost, c);
        break;
      }
    }
  }
}

std::string Us(double us) {
  std::ostringstream o;
  o << std::fixed << std::setprecision(us >= 100 ? 0 : 1);
  if (us >= 1e6) {
    o << std::setprecision(2) << us / 1e6 << " s";
  } else if (us >= 1000) {
    o << us / 1000 << " ms";
  } else {
    o << us << " us";
  }
  return o.str();
}

}  // namespace

Profitability EstimateProfitability(const KernelDecl& kernel, const PIRKernel& pir,
                                    const Schedule& schedule, const HardwareInfo& hw,
                                    const std::map<std::string, long long>& dims,
                                    const CostModelParams& p) {
  Profitability r;
  r.evaluated = true;
  r.dims = dims;
  std::ostringstream where;
  for (auto it = dims.begin(); it != dims.end(); ++it) where << (it == dims.begin() ? "" : ", ") << it->first << "=" << it->second;

  if (schedule.kind == ScheduleKind::SEQUENTIAL_ONLY) {
    r.reason = "not applicable: the loop is not safe to parallelize";
    return r;
  }
  Env env = dims;
  r.iterations = CountIterations(pir, 0, env);
  {
    Env e2 = dims;
    const LoopLevel& outer = pir.loop_nest.front();
    r.outer_trip = std::max<double>(0, static_cast<double>(EvalAffine(outer.upper, e2) - EvalAffine(outer.lower, e2)));
  }
  BodyCost body;
  CostStmts(kernel.body, p, pir.loop_nest.back().index_var, body);
  r.body_cycles = std::max(body.cycles, 1.0);
  double ns_per_cycle = 1.0 / p.clock_ghz;
  r.est_seq_us = r.iterations * r.body_cycles * ns_per_cycle / 1000.0;

  // Memory-bound detector (see below).
  double working_set = 0, total_elems = 0;
  auto add_array = [&](const Param& prm) {
    if (prm.type.IsScalar()) return;
    double elems = 1;
    for (const Dim& d : prm.type.dims) {
      elems *= d.is_ident ? static_cast<double>(dims.count(d.ident) ? dims.at(d.ident) : 1)
                          : static_cast<double>(d.int_value);
    }
    double bytes = prm.type.base == TypeBase::F64 || prm.type.base == TypeBase::I64 ? 8.0 : 4.0;
    working_set += elems * bytes;
    total_elems += elems;
  };
  for (const Param& prm : kernel.params) add_array(prm);
  bool ret_is_param = false;
  for (const Param& prm : kernel.params) ret_is_param |= prm.name == kernel.ret.name;
  if (!ret_is_param) add_array(kernel.ret);
  double bytes_per_cycle = body.refs * 4.0 / r.body_cycles;
  // Only streaming-style loops (little reuse per element) that spill the shared
  // last-level cache are treated as memory-bound.
  bool streaming = r.iterations <= 8.0 * std::max(total_elems, 1.0);
  r.memory_bound = working_set > static_cast<double>(hw.l3_bytes) && streaming &&
                   bytes_per_cycle >= p.memory_bound_bytes_per_cycle * 0.5 && r.body_cycles < 12;

  int max_t = std::max(1, std::min<int>(hw.logical_cpus, static_cast<int>(std::min(r.outer_trip, 1e6))));
  double best = r.est_seq_us;
  int best_t = 1;
  std::vector<std::pair<int, double>> options;
  for (int t = 2; t <= max_t; ++t) {
    double speed = t * p.parallel_efficiency;
    if (r.memory_bound) speed = std::min(speed, p.memory_bound_speedup_cap);
    speed = std::max(speed, 1.0);
    double over = p.fork_join_base_us + p.fork_join_per_thread_us * t;
    double est = over + r.est_seq_us / speed;
    options.push_back({t, est});
    best = std::min(best, est);
  }
  for (const auto& [t, est] : options) {
    if (est <= best * 1.05) { best_t = t; break; }  // smallest team within 5% of the best
  }
  r.threads = best_t;
  if (best_t > 1) {
    double speed = best_t * p.parallel_efficiency;
    if (r.memory_bound) speed = std::min(speed, p.memory_bound_speedup_cap);
    speed = std::max(speed, 1.0);
    r.overhead_us = p.fork_join_base_us + p.fork_join_per_thread_us * best_t;
    r.est_par_us = r.overhead_us + r.est_seq_us / speed;
    double denom = p.required_gain - 1.0 / speed;
    r.threshold_us = denom > 0 ? r.overhead_us / denom : 1e300;
  } else {
    r.est_par_us = r.est_seq_us;
    r.overhead_us = p.fork_join_base_us + p.fork_join_per_thread_us * 2;
    r.threshold_us = 1e300;
  }
  r.profitable = best_t > 1 && r.est_par_us <= p.required_gain * r.est_seq_us;

  std::ostringstream why;
  if (r.profitable) {
    why << "profitable at " << where.str() << ": estimated " << Us(r.est_seq_us) << " sequential vs "
        << Us(r.est_par_us) << " on " << best_t << " threads (fork/join overhead " << Us(r.overhead_us)
        << "; break-even work " << Us(r.threshold_us) << ")"
        << (r.memory_bound ? "; memory-bound loop, speedup capped" : "");
  } else if (hw.logical_cpus < 2) {
    why << "not profitable at " << where.str() << ": only 1 logical CPU detected";
  } else if (r.outer_trip < 2) {
    why << "not profitable at " << where.str() << ": the parallel loop runs fewer than 2 iterations";
  } else {
    why << "not profitable at " << where.str() << ": estimated work " << Us(r.est_seq_us)
        << " is below the threshold of " << Us(r.threshold_us) << " needed to repay the "
        << Us(r.overhead_us) << " parallel overhead";
  }
  r.reason = why.str();
  return r;
}

}  // namespace parallix
