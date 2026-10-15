#include "parallix/compiler_driver.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>

namespace parallix {

namespace {

// Runs `cmd` via the shell, capturing combined stdout+stderr, and returns the
// process's exit code (or -1 if it could not be waited on normally).
int RunCaptured(const std::string& cmd, std::string* output) {
  FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
  if (!pipe) {
    *output = "failed to launch subprocess";
    return -1;
  }
  std::array<char, 4096> buf;
  size_t n;
  while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0) {
    output->append(buf.data(), n);
  }
  int status = pclose(pipe);
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
}

bool LooksLikeMissingCompiler(int exit_code, const std::string& output) {
  return exit_code == 127 || output.find("not found") != std::string::npos ||
         output.find("No such file or directory") != std::string::npos;
}

}  // namespace

int RunCommand(const std::string& cmd, std::string* output) { return RunCaptured(cmd, output); }

CompileResult CompileC(const std::string& source_path, const std::string& binary_path,
                       const std::string& extra_flags, bool want_openmp) {
  CompileResult result;
  const char* candidates[] = {"cc", "gcc"};
  bool tried_any = false;
  for (const char* compiler : candidates) {
    for (int attempt = 0; attempt < (want_openmp ? 2 : 1); ++attempt) {
      bool openmp = want_openmp && attempt == 0;
      const char* env_flags = std::getenv("PARALLIX_CFLAGS");  // e.g. sanitizers for generated code
      std::string cmd = std::string(compiler) + " -O2 -fwrapv " + (openmp ? "-fopenmp " : "") +
                        extra_flags + " " + (env_flags ? env_flags : "") + " \"" + source_path + "\" -o \"" + binary_path + "\" -lm";
      std::string output;
      int code = RunCaptured(cmd, &output);
      if (LooksLikeMissingCompiler(code, output) && output.find("error") == std::string::npos) {
        break;  // this compiler is not installed
      }
      tried_any = true;
      result.compiler = compiler;
      result.output = output;
      if (code == 0) {
        result.ok = true;
        result.openmp = openmp;
        std::string ver;
        RunCaptured(std::string(compiler) + " --version | head -1", &ver);
        while (!ver.empty() && (ver.back() == '\n' || ver.back() == '\r')) ver.pop_back();
        result.version = ver;
        return result;
      }
    }
    if (tried_any) break;
  }
  result.error = tried_any ? "compilation failed with '" + result.compiler + "':\n" + result.output
                           : "no C compiler found on PATH: tried cc, gcc";
  return result;
}

CompileRunResult CompileAndRun(const std::string& source_path, const std::string& binary_path) {
  CompileRunResult result;

  const char* candidates[] = {"cc", "gcc"};
  for (const char* compiler : candidates) {
    std::string cmd =
        std::string(compiler) + " -O2 -fwrapv -fopenmp \"" + source_path + "\" -o \"" + binary_path + "\" -lm";
    std::string output;
    int exit_code = RunCaptured(cmd, &output);
    if (LooksLikeMissingCompiler(exit_code, output)) {
      continue;  // try the next candidate
    }
    result.compiler_found = true;
    result.compiler_used = compiler;
    result.compile_output = output;
    result.compiled = (exit_code == 0);
    if (!result.compiled) {
      result.error = "compilation failed with '" + std::string(compiler) + "':\n" + output;
      return result;
    }
    break;
  }

  if (!result.compiler_found) {
    result.error = "no C compiler found on PATH: tried cc, gcc";
    return result;
  }

  std::string run_output;
  int exit_code = RunCaptured("\"" + binary_path + "\"", &run_output);
  result.ran = true;
  result.exit_code = exit_code;
  result.run_output = run_output;
  return result;
}

}  // namespace parallix
