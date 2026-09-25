#pragma once

#include <string>

namespace parallix {

// Backend A's only remaining step: actually invoking the system C compiler
// and actually running the result (specs/06 section 3) — the pipeline never
// stops at emitting source.
struct CompileRunResult {
  bool compiler_found = false;
  std::string compiler_used;  // "cc" or "gcc", valid iff compiler_found

  bool compiled = false;
  std::string compile_output;  // combined stdout+stderr of the compile step

  bool ran = false;
  int exit_code = 0;
  std::string run_output;  // combined stdout+stderr of running the binary

  // Non-empty iff something went wrong badly enough to stop (no compiler
  // found, or the compile step itself failed) — a clear, named error, never
  // a bare failure code (constitution rule 5 applies here too).
  std::string error;

  bool Success() const { return compiled && ran && exit_code == 0 && error.empty(); }
};

// Compiles `source_path` (a self-contained .c file) into `binary_path` with
// `cc -fopenmp`, falling back to `gcc -fopenmp` if `cc` is missing or not the
// one found, then runs the resulting binary and captures its output.
CompileRunResult CompileAndRun(const std::string& source_path, const std::string& binary_path);

}  // namespace parallix
