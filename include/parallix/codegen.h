#pragma once

#include <string>
#include <vector>

#include "parallix/ast.h"
#include "parallix/ir.h"
#include "parallix/schedule.h"

namespace parallix {

// Describes one array parameter of the generated C function, in generated
// signature order, for use both by codegen itself and by the harness
// generator (M9), which needs to know how to allocate and fill it.
struct ArrayParamInfo {
  std::string name;
  TypeBase elem_type;
  std::vector<Dim> dims;  // declared shape, outer-to-inner (row-major)
  bool is_output = false;  // true for the kernel's returned array
  bool is_input = false;   // true if read anywhere as an input
};

// Everything Backend A (specs/06 section 2) produces for one kernel: the C
// source for its sequential (always) and OpenMP (iff schedule !=
// SEQUENTIAL_ONLY) function definitions, plus enough structural metadata for
// the harness generator to call them correctly.
struct CodegenResult {
  std::string kernel_name;
  std::string c_source;  // function definitions only, no #include or main()

  std::string seq_function_name;
  std::string par_function_name;  // empty iff schedule.kind == SEQUENTIAL_ONLY

  bool return_is_array = false;
  std::string return_c_type;      // "void" if return_is_array, else the scalar C type
  std::string return_array_name;  // valid iff return_is_array

  std::vector<ArrayParamInfo> arrays;  // every array in the generated signature
  std::vector<std::string> dim_params;  // int params, first-encountered order

  bool is_reduction = false;
  ReduceOp reduce_op = ReduceOp::ADD;
  std::string reduce_accumulator;
};

const char* CTypeName(TypeBase base);

// Generates Backend A C source for `kernel` per specs/06 section 2, following
// `schedule` (from M7's GenerateSchedule) for pragma placement / which
// functions to emit. `kernel` must already have passed Sema; `pir` is its
// already-lowered PIR (for loop-level indices/modes and reduction metadata).
CodegenResult GenerateCCode(const KernelDecl& kernel, const PIRKernel& pir,
                            const Schedule& schedule);

}  // namespace parallix
