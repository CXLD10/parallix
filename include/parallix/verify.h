#pragma once

#include <map>
#include <string>
#include <vector>

#include "parallix/compiler_driver.h"
#include "parallix/pipeline.h"
#include "parallix/witness.h"

namespace parallix {

// One line of `PLX key=value ...` output from a generated harness program.
struct PlxLine {
  std::string kind;                         // word after "PLX"
  std::map<std::string, std::string> kv;
  std::string Get(const std::string& key, const std::string& fallback = "") const {
    auto it = kv.find(key);
    return it == kv.end() ? fallback : it->second;
  }
  long long GetInt(const std::string& key, long long fallback = 0) const;
  double GetDouble(const std::string& key, double fallback = 0) const;
};
std::vector<PlxLine> ParsePlxOutput(const std::string& output);

// A generated, compiled harness program.
struct BuiltHarness {
  std::string c_path;
  std::string bin_path;
  CompileResult compile;
};
BuiltHarness BuildHarness(const Analysis& a, const CodegenResult& gen, const Schedule& schedule,
                          const std::string& dir, const std::string& tag,
                          const std::string& extra_flags = "");

struct ValidationOptions {
  std::vector<int> sizes = {37, 101, 257};
  std::vector<unsigned> seeds = {1, 2, 3};
  std::vector<int> threads;  // empty: 1, 2, 4, 8 and the machine maximum
  int reps = 20;
  int perms = 3;
  bool tsan = true;
  std::string work_dir = "generated";
  std::vector<std::pair<std::string, double>> scalars;  // --scalar overrides
};

struct ValidationCell {
  std::string variant;  // "reverse", "perm1".., "par"
  int size = 0;
  unsigned seed = 0;
  int threads = 0;  // 0 for the sequential order variants
  int team = 0;     // threads OpenMP actually provided (par only)
  int runs = 0;
  int mismatches = 0;
};

struct ValidationReport {
  bool built = false;
  std::string error;
  std::string compiler, compiler_version;
  bool openmp = false;
  int max_threads = 0;
  bool has_par = false;

  std::vector<int> sizes, threads;
  std::vector<unsigned> seeds;
  int reps = 0, perms = 0;

  std::vector<ValidationCell> cells;
  long long order_runs = 0, order_mismatches = 0, par_runs = 0, par_mismatches = 0;
  std::string first_mismatch;  // raw detail line, empty if none

  // ThreadSanitizer: "not-requested", "clean", "races", or "skipped".
  std::string tsan_status = "not-requested";
  std::string tsan_detail;

  bool Passed() const { return built && order_mismatches == 0 && par_mismatches == 0; }
};

// Compiles and runs the order-independence and multi-thread validation.
ValidationReport RunValidation(const Analysis& a, const ValidationOptions& opts);

// Human-readable lines describing exactly what was covered (and nothing more).
std::string FormatValidation(const Analysis& a, const ValidationReport& r);

// ---- counterexamples for UNSAFE kernels ---------------------------------

struct RefutationOptions {
  int size = 12;
  unsigned seed = 1;
  bool force_parallel = false;
  int forced_reps = 200;
  std::string work_dir = "generated";
  std::vector<std::pair<std::string, double>> scalars;
};

struct RefutationReport {
  bool built = false;
  std::string error;
  int size = 0;
  std::vector<Witness> witnesses;  // one per dependent reference pair

  // Deterministic proof: reversed iteration order changes the result.
  bool diverged = false;
  unsigned seed = 0;
  long long index = -1;  // flat output index, -1 for a scalar result
  std::string location;  // e.g. "dp[3]" or "result"
  double sequential_value = 0, reversed_value = 0;

  // Nondeterministic demonstration (only with --force-parallel).
  bool forced = false;
  int forced_threads = 0, forced_team = 0, forced_runs = 0, forced_differing = 0;

  bool Refuted() const { return built && diverged && !witnesses.empty() && witnesses.front().found; }
};

RefutationReport RunRefutation(const Analysis& a, const RefutationOptions& opts);
std::string FormatRefutation(const Analysis& a, const RefutationReport& r);

}  // namespace parallix
