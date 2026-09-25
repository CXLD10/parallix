#pragma once

#include <string>

#include "parallix/ast.h"
#include "parallix/codegen.h"
#include "parallix/ir.h"
#include "parallix/schedule.h"

namespace parallix {

// First working version of proof-carrying verification (specs/06 section 4):
// a small, deterministic, self-contained C program (includes + the
// already-generated function definitions + a `main`) that either certifies
// agreement between the sequential and OpenMP variants, or runs a
// sequential-only smoke test for an UNSAFE kernel that has no parallel
// variant to compare against.
struct HarnessResult {
  std::string c_source;     // a complete, compilable .c file, including main()
  bool attempts_certificate;  // false for UNSAFE (schedule.kind == SEQUENTIAL_ONLY)
};

// `gen` must be the CodegenResult produced by GenerateCCode for the same
// kernel/pir/schedule triple — the harness embeds `gen.c_source` verbatim and
// calls its functions by the names/signatures GenerateCCode chose.
HarnessResult GenerateHarness(const KernelDecl& kernel, const PIRKernel& pir,
                              const Schedule& schedule, const CodegenResult& gen);

}  // namespace parallix
