#include "parallix/dependence.h"

#include <numeric>
#include <set>
#include <sstream>

namespace parallix {

namespace {

PairResult AnalyzePair(const std::vector<Reference>& refs, size_t ia, size_t ib) {
  PairResult pr;
  pr.ref_a_index = ia;
  pr.ref_b_index = ib;
  const Reference& a = refs[ia];
  const Reference& b = refs[ib];

  // Step 1 — same-array filter.
  if (a.array_name != b.array_name) {
    pr.dependent = false;
    pr.reasoning = "different arrays ('" + a.array_name + "' vs '" + b.array_name +
                    "') -> independent (Step 1)";
    return pr;
  }
  if (a.kind == RefKind::READ && b.kind == RefKind::READ) {
    pr.dependent = false;
    pr.reasoning = "both references read '" + a.array_name + "' -> independent (Step 1)";
    return pr;
  }

  bool both_writes = (a.kind == RefKind::WRITE && b.kind == RefKind::WRITE);
  // For a write/read pair, always compare as (write, read) so the distance
  // formula (specs/04 step 3: distance = b1 - b2, write's offset minus
  // read's offset) has a fixed, well-defined meaning.
  size_t idx_w1 = (a.kind == RefKind::WRITE) ? ia : ib;
  size_t idx_w2_or_read = (a.kind == RefKind::WRITE) ? ib : ia;
  const Reference& write_ref = refs[idx_w1];
  const Reference& other_ref = refs[idx_w2_or_read];

  // Step 2 — GCD test, per dimension (the same algorithm as the 1-D case in
  // specs/04, generalized by requiring every dimension to admit a solution —
  // ruling out any single dimension rules out the whole pair).
  size_t dims = write_ref.access.size();
  std::ostringstream gcd_trace;
  for (size_t d = 0; d < dims; ++d) {
    long long a1 = write_ref.access[d].coefficient;
    long long a2 = other_ref.access[d].coefficient;
    long long b1 = write_ref.access[d].constant;
    long long b2 = other_ref.access[d].constant;
    long long g = std::gcd(a1, a2);
    long long diff = b2 - b1;
    bool divides = (g == 0) ? (diff == 0) : (diff % g == 0);
    if (!divides) {
      pr.dependent = false;
      pr.reasoning = "same array '" + a.array_name + "': GCD test rules out any integer "
                      "solution in dimension " + std::to_string(d) + " (gcd(" +
                      std::to_string(a1) + ", " + std::to_string(a2) + ") = " +
                      std::to_string(g) + " does not divide " + std::to_string(diff) +
                      ") -> independent (Step 2)";
      return pr;
    }
  }

  // Step 3 — distance/direction (unit-coefficient case).
  std::vector<size_t> varying_dims;
  std::set<std::string> distinct_indices;
  for (size_t d = 0; d < dims; ++d) {
    bool varies = write_ref.access[d].coefficient != 0 || other_ref.access[d].coefficient != 0;
    if (!varies) continue;
    varying_dims.push_back(d);
    if (write_ref.access[d].coefficient != 0) distinct_indices.insert(write_ref.access[d].index_var);
    if (other_ref.access[d].coefficient != 0) distinct_indices.insert(other_ref.access[d].index_var);
  }

  bool distance_known = false;
  long long distance = 0;
  std::string distance_note;
  if (distinct_indices.empty()) {
    // No varying dimension: both references touch the exact same fixed
    // element every iteration (loop-independent).
    distance_known = true;
    distance = 0;
    distance_note = "no varying dimension -> loop-independent";
  } else if (distinct_indices.size() > 1) {
    distance_note = "dependence spans more than one loop index across dimensions "
                     "(out of Phase 1's distance-computation scope)";
  } else {
    size_t d = varying_dims.front();
    long long a1 = write_ref.access[d].coefficient;
    long long a2 = other_ref.access[d].coefficient;
    if (a1 == 1 && a2 == 1) {
      distance_known = true;
      distance = write_ref.access[d].constant - other_ref.access[d].constant;
      distance_note = "unit-coefficient case: distance = b1 - b2 = " +
                       std::to_string(write_ref.access[d].constant) + " - " +
                       std::to_string(other_ref.access[d].constant) + " = " +
                       std::to_string(distance);
    } else {
      distance_note = "non-unit coefficient(s) in dimension " + std::to_string(d) + " (a1=" +
                       std::to_string(a1) + ", a2=" + std::to_string(a2) +
                       ") -> distance/direction unknown (documented Phase 1 simplification, "
                       "specs/04 step 3)";
    }
  }

  pr.dependent = true;
  pr.distance_known = distance_known;
  pr.distance = distance;

  // Step 4 — classification.
  if (both_writes) {
    pr.type = DependenceType::WAW;
  } else if (!distance_known) {
    // Direction can't be determined without a distance; conservatively
    // report it as a loop-carried true dependence (the common real-world
    // case) while saying plainly that direction is unresolved.
    pr.type = DependenceType::RAW;
  } else if (distance > 0) {
    pr.type = DependenceType::RAW;
  } else if (distance < 0) {
    pr.type = DependenceType::WAR;
  } else {
    // distance == 0: same-iteration overlap; direction follows program
    // order, which the reference list already reflects (lowering appends
    // references in source order).
    pr.type = (idx_w1 < idx_w2_or_read) ? DependenceType::RAW : DependenceType::WAR;
  }

  std::ostringstream reason;
  reason << "same array '" << a.array_name << "', GCD test admits a solution in every "
         << "dimension (Step 2); " << distance_note << " (Step 3); classified as "
         << ToString(pr.type) << " (Step 4)";
  pr.reasoning = reason.str();
  return pr;
}

}  // namespace

DependenceReport AnalyzeDependence(const PIRKernel& kernel) {
  DependenceReport report;
  const auto& refs = kernel.references;
  for (size_t i = 0; i < refs.size(); ++i) {
    for (size_t j = i + 1; j < refs.size(); ++j) {
      report.pairs.push_back(AnalyzePair(refs, i, j));
    }
  }

  bool any_unsafe = false;
  for (const auto& pr : report.pairs) {
    if (pr.dependent && pr.type != DependenceType::NONE) {
      any_unsafe = true;
      break;
    }
  }

  if (any_unsafe) {
    report.verdict = Verdict::UNSAFE;
    report.verdict_reason = "at least one loop-carried array dependence was found that is "
                             "not a licensed reduction";
  } else if (kernel.is_reduction) {
    report.verdict = Verdict::SAFE_AS_REDUCTION;
    report.verdict_reason = "the only loop-carried dependence is on the scalar accumulator '" +
                             kernel.reduce_accumulator + "', combined exclusively with its "
                             "declared operator '" + ToString(kernel.reduce_op) +
                             "' inside reduce(" + ToString(kernel.reduce_op) +
                             ") { } (Step 5) — licensed as a parallel reduction, "
                             "assuming operator associativity (note: floating-point " +
                             ToString(kernel.reduce_op) +
                             " reduction may reorder rounding, so results may not be "
                             "bit-exact across schedules)";
  } else {
    report.verdict = Verdict::SAFE;
    report.verdict_reason = "no dependence was found between any pair of references";
  }

  return report;
}

}  // namespace parallix
