#pragma once

#include <string>
#include <utility>
#include <vector>

#include "parallix/hardware.h"
#include "parallix/pipeline.h"

namespace parallix {

// The decision made for one loop of the nest: which rule applied, the
// evidence behind it, and the alternatives that were considered and rejected.
struct LoopDecision {
  std::string loop;       // index variable
  std::string header;     // e.g. "auto i in [0, N)"
  std::string outcome;    // "parallelized (OpenMP)", "kept sequential", ...
  std::string rule;       // stable rule name
  std::string rule_text;  // what the rule says
  std::vector<std::string> evidence;
  std::vector<std::pair<std::string, std::string>> rejected;  // alternative, why not
};

struct Explanation {
  std::string kernel, verdict, verdict_reason, schedule_summary;
  std::vector<std::string> notes;  // kernel-level remarks (annotations overridden, ...)
  std::vector<LoopDecision> loops;
};

// `size` is the problem size the cost model reasons about.
Explanation Explain(const Analysis& a, const HardwareInfo& hw, long long size);
std::string ExplainText(const Explanation& e);
// A single self-contained HTML page (no external assets).
std::string ExplainHtml(const Explanation& e, const std::string& source_text);

}  // namespace parallix
