#pragma once

#include <string>
#include <vector>

#include "parallix/ast.h"
#include "parallix/ir.h"
#include "parallix/schedule.h"

namespace parallix {

// Describes one array parameter of the generated C function, in generated
// signature order, for use both by codegen itself and by the harness
// generator, which needs to know how to allocate and fill it.
struct ArrayParamInfo {
  std::string name;
  TypeBase elem_type;
  std::vector<Dim> dims;  // declared shape, outer-to-inner (row-major)
  bool is_output = false;  // true for the kernel's returned array
  bool is_input = false;   // true if read anywhere as an input
};

struct ScalarParamInfo {
  std::string name;
  TypeBase type;
};

// One parameter of the generated C signature, in signature order.
struct ArgSlot {
  enum Kind { ARRAY, SCALAR, DIM };
  std::string name;
  Kind kind;
};

// Everything Backend A produces for one kernel: the C
// source for its sequential (always) and OpenMP (iff schedule !=
// SEQUENTIAL_ONLY) function definitions, plus enough structural metadata for
// the harness generator to call them correctly.
struct CodegenResult {
  std::string kernel_name;
  std::string c_source;  // function definitions only, no #include or main()

  std::string seq_function_name;
  std::string par_function_name;  // empty iff schedule.kind == SEQUENTIAL_ONLY
  // Sequential variant whose outermost loop runs its iterations in the order
  // given by a trailing `const int* plx_order` argument (a permutation of
  // 0..trip-1). Always generated; used to test order independence and to
  // demonstrate counterexamples.
  std::string ord_function_name;
  std::string trip_count_c;  // C expression (over dim params) for the outermost loop's trip count

  bool return_is_array = false;
  TypeBase return_base = TypeBase::F32;  // element/scalar type of the kernel's result
  std::string return_c_type;      // "void" if return_is_array, else the scalar C type
  std::string return_array_name;  // valid iff return_is_array

  std::vector<ArrayParamInfo> arrays;  // every array in the generated signature
  std::vector<ScalarParamInfo> scalars;  // scalar (non-array) kernel parameters
  std::vector<ArgSlot> arg_order;        // full signature order: arrays, scalars, then dims
  std::vector<std::string> dim_params;  // int params, first-encountered order

  bool is_reduction = false;
  ReduceOp reduce_op = ReduceOp::ADD;
  std::string reduce_accumulator;
};

const char* CTypeName(TypeBase base);

// Generates Backend A C source for `kernel`, following
// `schedule` (from GenerateSchedule) for pragma placement / which
// functions to emit. `kernel` must already have passed Sema; `pir` is its
// already-lowered PIR (for loop-level indices/modes and reduction metadata).
CodegenResult GenerateCCode(const KernelDecl& kernel, const PIRKernel& pir,
                            const Schedule& schedule);

}  // namespace parallix
