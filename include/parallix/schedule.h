#pragma once

#include <string>

#include "parallix/dependence.h"
#include "parallix/ir.h"

namespace parallix {

// Phase 2's schedule generator is deliberately the simplest thing that can
// exist: a fixed rule per verdict, no cost model, no search (specs/06,
// section 1). A cost model / search over tile factors and permutations is
// explicitly later-phase work.
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

struct Schedule {
  ScheduleKind kind;
  // Index into PIRKernel::loop_nest naming the loop level to parallelize.
  // Meaningless (0) for SEQUENTIAL_ONLY.
  size_t parallel_loop_index = 0;
  std::string reason;  // human-readable, names the verdict that drove this choice
};

// Applies specs/06's fixed verdict -> schedule table:
//   SAFE               -> PARALLEL_OUTER on loop_nest[0]
//   SAFE_AS_REDUCTION  -> PARALLEL_REDUCTION on loop_nest[0] (the reduction
//                         loop; Phase 1's PIR only ever has one loop level
//                         for a reduction kernel)
//   UNSAFE             -> SEQUENTIAL_ONLY
Schedule GenerateSchedule(const PIRKernel& kernel, const DependenceReport& report);

}  // namespace parallix
