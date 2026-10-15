#pragma once

#include <map>
#include <string>

#include "parallix/ast.h"
#include "parallix/dependence.h"
#include "parallix/hardware.h"
#include "parallix/ir.h"

namespace parallix {

// The base schedule is a fixed rule per verdict; the cost model below then
// decides whether parallelizing pays off at a given problem size. There is no
// search over tile factors or permutations.
enum class ScheduleKind {
  SEQUENTIAL_ONLY,     // UNSAFE: no parallel variant is generated at all.
  PARALLEL_OUTER,      // SAFE: OpenMP `parallel for` on the outermost loop.
  PARALLEL_REDUCTION,  // SAFE_AS_REDUCTION: OpenMP `parallel for
                        // reduction(op:accumulator)` on the reduction loop.
};

inline const char* ToString(ScheduleKind kind) {
  switch (kind) {
    case ScheduleKind::SEQUENTIAL_ONLY: return "sequential-only";
    case ScheduleKind::PARALLEL_OUTER: return "parallel-outer";
    case ScheduleKind::PARALLEL_REDUCTION: return "parallel-reduction";
  }
  return "?";
}

// Output of the cost model (see EstimateProfitability).
struct Profitability {
  bool evaluated = false;
  std::map<std::string, long long> dims;  // problem size the estimate is for
  double iterations = 0;                  // body executions in the whole nest
  double body_cycles = 0;                 // estimated cycles per body execution
  double outer_trip = 0;
  double est_seq_us = 0;
  double est_par_us = 0;      // at `threads`
  double overhead_us = 0;     // fork/join cost at `threads`
  double threshold_us = 0;    // smallest sequential time worth parallelizing at `threads`
  bool memory_bound = false;
  int threads = 1;            // recommended team size
  bool profitable = false;
  std::string reason;         // human-readable, always set once evaluated
};

struct Schedule {
  ScheduleKind kind;
  Profitability profit;  // empty until ApplyCostModel runs
  // Index into PIRKernel::loop_nest naming the loop level to parallelize.
  // Meaningless (0) for SEQUENTIAL_ONLY.
  size_t parallel_loop_index = 0;
  // OpenMP schedule clause body ( = runtime default, i.e. static). Triangular
  // nests (an inner bound depends on an outer index) get dynamic scheduling so
  // early iterations, which carry more work, do not unbalance the threads.
  std::string omp_schedule;
  std::string reason;  // human-readable, names the verdict that drove this choice
};

// Applies the fixed verdict -> schedule table:
//   SAFE               -> PARALLEL_OUTER on loop_nest[0]
//   SAFE_AS_REDUCTION  -> PARALLEL_REDUCTION on loop_nest[0] (the reduction
//                         loop; the PIR only ever has one loop level
//                         for a reduction kernel)
//   UNSAFE             -> SEQUENTIAL_ONLY
Schedule GenerateSchedule(const PIRKernel& kernel, const DependenceReport& report);

}  // namespace parallix

namespace parallix {

// Documented constants of the cost model. All are order-of-magnitude
// estimates; `bench` prints the model's prediction next to the measurement so
// the error is visible rather than hidden.
struct CostModelParams {
  double clock_ghz = 3.0;            // cycles -> time
  double cycles_per_op = 1.0;        // + - * compare select min max
  double cycles_per_div = 8.0;
  double cycles_per_ref = 1.5;       // one array element load or store (L1 hit)
  double fork_join_base_us = 12.0;   // cost of entering/leaving a parallel region
  double fork_join_per_thread_us = 1.5;
  double parallel_efficiency = 0.85; // fraction of ideal speedup for compute-bound loops
  double memory_bound_speedup_cap = 4.0;  // when bytes/cycle is high, extra threads stop helping
  double memory_bound_bytes_per_cycle = 1.0;
  double required_gain = 0.8;        // parallelize only if est_par <= required_gain * est_seq
};

// Estimates sequential vs parallel time for the schedule's loop at the given
// dimension values, picks the team size (smallest within 5% of the best), and
// decides whether parallelizing pays for its overhead. Does not change
// `schedule.kind`; a loop the analysis proved safe stays safe but may be
// declined as unprofitable at this size.
Profitability EstimateProfitability(const KernelDecl& kernel, const PIRKernel& pir,
                                    const Schedule& schedule, const HardwareInfo& hw,
                                    const std::map<std::string, long long>& dims,
                                    const CostModelParams& params = CostModelParams());

}  // namespace parallix
