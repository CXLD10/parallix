#pragma once

#include <string>
#include <vector>

#include "parallix/ast.h"
#include "parallix/codegen.h"
#include "parallix/dependence.h"
#include "parallix/ir.h"
#include "parallix/schedule.h"
#include "parallix/token.h"

namespace parallix {

// Everything the front half of the compiler knows about one kernel.
struct Analysis {
  KernelDecl kernel;
  PIRKernel pir;
  DependenceReport report;
  Schedule schedule;
  CodegenResult gen;
};

// A parsed source file: its text, tokens and kernels (not yet analysed).
struct ParsedSource {
  std::string text;
  std::vector<Token> tokens;
  Program program;
};

// Reads and parses `path`. On failure returns false and sets `error` to a
// message that starts with the stage ("lex error:" / "parse error:" / "error:").
bool ParseSourceFile(const std::string& path, ParsedSource* out, std::string* error);

// Semantic analysis, lowering, dependence analysis, scheduling and code
// generation for one kernel. On a semantic error returns false and sets
// `error` to the named diagnostic.
bool AnalyzeKernelDecl(KernelDecl kernel, Analysis* out, std::string* error);

// Regenerates the C code for an analysed kernel under a different schedule
// (used to force a parallel variant of an UNSAFE kernel for the counterexample
// demo; nothing about the verdict changes).
CodegenResult GenerateWith(const Analysis& analysis, const Schedule& schedule);

// SHA-256 of the exact source text, lowercase hex.
std::string SourceHash(const std::string& text);

}  // namespace parallix
