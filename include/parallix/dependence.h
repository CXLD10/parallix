#pragma once

#include <string>
#include <vector>

#include "parallix/ir.h"

namespace parallix {

enum class DependenceType { NONE, RAW, WAR, WAW };

inline const char* ToString(DependenceType type) {
  switch (type) {
    case DependenceType::NONE: return "no dependence";
    case DependenceType::RAW: return "RAW";
    case DependenceType::WAR: return "WAR";
    case DependenceType::WAW: return "WAW";
  }
  return "?";
}

enum class Verdict { SAFE, UNSAFE, SAFE_AS_REDUCTION };

inline const char* ToString(Verdict verdict) {
  switch (verdict) {
    case Verdict::SAFE: return "SAFE";
    case Verdict::UNSAFE: return "UNSAFE";
    case Verdict::SAFE_AS_REDUCTION: return "SAFE_AS_REDUCTION";
  }
  return "?";
}

// The result of comparing one pair of references (specs/04, steps 1-4).
struct PairResult {
  size_t ref_a_index;
  size_t ref_b_index;
  bool dependent = false;  // false => ruled out at Step 1 or Step 2
  DependenceType type = DependenceType::NONE;
  bool distance_known = false;
  long long distance = 0;
  std::string reasoning;  // human-readable, names the step that resolved this pair
};

struct DependenceReport {
  std::vector<PairResult> pairs;  // one entry per reference pair considered
  Verdict verdict;
  std::string verdict_reason;
};

// Runs the dependence engine (specs/04) over every reference pair in the
// kernel's PIR, then derives the overall verdict (Step 6), folding in the
// reduction special case (Step 5) via `kernel.is_reduction`.
DependenceReport AnalyzeDependence(const PIRKernel& kernel);

}  // namespace parallix
