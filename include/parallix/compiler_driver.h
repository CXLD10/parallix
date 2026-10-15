#pragma once

#include <string>

namespace parallix {

// Backend A's only remaining step: actually invoking the system C compiler
// and actually running the result — the pipeline never
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
  // a bare failure code (every failure names its cause).
  std::string error;

  bool Success() const { return compiled && ran && exit_code == 0 && error.empty(); }
};

// Result of compiling one C file.
struct CompileResult {
  bool ok = false;
  std::string compiler;  // "cc" or "gcc"
  std::string version;   // first line of `<compiler> --version`
  bool openmp = false;   // true if built with -fopenmp
  std::string output;    // compiler diagnostics
  std::string error;     // non-empty on failure
};

// Runs `cmd` through the shell capturing stdout+stderr; returns its exit code
// (or -1 if it did not exit normally).
int RunCommand(const std::string& cmd, std::string* output);

// Compiles with `-O2 -fwrapv` plus `extra_flags`. Tries cc then gcc. If
// `want_openmp` and the OpenMP build fails, retries without OpenMP (the
// generated programs have single-thread stubs) and reports openmp=false.
CompileResult CompileC(const std::string& source_path, const std::string& binary_path,
                       const std::string& extra_flags, bool want_openmp);

// Compiles `source_path` (a self-contained .c file) into `binary_path` with
// `cc -fopenmp`, falling back to `gcc -fopenmp` if `cc` is missing or not the
// one found, then runs the resulting binary and captures its output.
CompileRunResult CompileAndRun(const std::string& source_path, const std::string& binary_path);

}  // namespace parallix
