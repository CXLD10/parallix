#include "parallix/schedule.h"

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
  return schedule;
}

}  // namespace parallix
