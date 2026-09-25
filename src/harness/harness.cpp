#include "parallix/harness.h"

#include <sstream>
#include <vector>

namespace parallix {

namespace {

// Fixed test sizes for symbolic array dimensions, assigned in
// first-encountered order (N, M, ...). Deliberately small so the harness
// compiles and runs fast, but large enough to give OpenMP's thread split a
// real chance to reorder a reduction's partial sums (specs/06 section 4).
const int kDimTestValues[] = {64, 48, 24, 16, 12, 8};

std::string DimTestValue(size_t position) {
  size_t n = sizeof(kDimTestValues) / sizeof(kDimTestValues[0]);
  return std::to_string(kDimTestValues[position < n ? position : n - 1]);
}

// A runtime C expression computing an array's total element count, e.g.
// "(size_t)N * (size_t)M" for a 2-D array.
std::string CountExpr(const std::vector<Dim>& dims) {
  std::ostringstream out;
  for (size_t i = 0; i < dims.size(); ++i) {
    if (i > 0) out << " * ";
    if (dims[i].is_ident) {
      out << "(size_t)" << dims[i].ident;
    } else {
      out << dims[i].int_value;
    }
  }
  return out.str();
}

std::string FillFunctionFor(TypeBase base) {
  switch (base) {
    case TypeBase::F32: return "fill_seeded_float";
    case TypeBase::F64: return "fill_seeded_double";
    case TypeBase::I32: return "fill_seeded_int";
  }
  return "fill_seeded_float";
}

// Common preamble every generated harness needs: includes, the seeded-fill
// helpers (one per element type Parallix supports; emitted unconditionally
// for simplicity, unused ones are harmless), and the kernel's own generated
// function definitions.
std::string Preamble(const CodegenResult& gen) {
  std::ostringstream out;
  out << "#include <stdio.h>\n"
         "#include <stdlib.h>\n"
         "#include <math.h>\n"
         "#ifdef _OPENMP\n"
         "#include <omp.h>\n"
         "#endif\n\n"
         "// Deterministic, seeded fill: same formula -> same values every run, so "
         "the\n"
         "// sequential and OpenMP variants are always compared on identical input "
         "(specs/06\n"
         "// section 4 -- repeatability, not hidden luck).\n"
         "// Not every generated harness uses all three element types.\n"
         "#if defined(__GNUC__) || defined(__clang__)\n"
         "#define PARALLIX_MAYBE_UNUSED __attribute__((unused))\n"
         "#else\n"
         "#define PARALLIX_MAYBE_UNUSED\n"
         "#endif\n"
         "static PARALLIX_MAYBE_UNUSED void fill_seeded_float(float* buf, size_t n, "
         "unsigned seed_offset) {\n"
         "  for (size_t i = 0; i < n; i++) {\n"
         "    unsigned x = (unsigned)(i + seed_offset) * 2654435761u;\n"
         "    buf[i] = (float)(x % 1000) / 10.0f;\n"
         "  }\n"
         "}\n"
         "static PARALLIX_MAYBE_UNUSED void fill_seeded_double(double* buf, size_t n, "
         "unsigned seed_offset) {\n"
         "  for (size_t i = 0; i < n; i++) {\n"
         "    unsigned x = (unsigned)(i + seed_offset) * 2654435761u;\n"
         "    buf[i] = (double)(x % 1000) / 10.0;\n"
         "  }\n"
         "}\n"
         "static PARALLIX_MAYBE_UNUSED void fill_seeded_int(int* buf, size_t n, unsigned "
         "seed_offset) {\n"
         "  for (size_t i = 0; i < n; i++) {\n"
         "    unsigned x = (unsigned)(i + seed_offset) * 2654435761u;\n"
         "    buf[i] = (int)(x % 1000);\n"
         "  }\n"
         "}\n\n"
      << gen.c_source << "\n";
  return out.str();
}

std::string BuildCertifyingHarness(const KernelDecl& kernel, const Schedule& schedule,
                                   const CodegenResult& gen) {
  (void)schedule;
  std::ostringstream out;
  out << Preamble(gen);

  out << "int main(void) {\n";
  // Declare and fill dimension sizes.
  for (size_t i = 0; i < gen.dim_params.size(); ++i) {
    out << "  int " << gen.dim_params[i] << " = " << DimTestValue(i) << ";\n";
  }
  out << "\n";

  // Input arrays: one shared buffer feeds both variants (specs/06: same seed
  // for both runs).
  unsigned seed_offset = 0;
  for (const ArrayParamInfo& arr : gen.arrays) {
    if (!arr.is_input) continue;
    std::string count = CountExpr(arr.dims);
    out << "  " << CTypeName(arr.elem_type) << "* " << arr.name << " = malloc((" << count
        << ") * sizeof(" << CTypeName(arr.elem_type) << "));\n";
    out << "  " << FillFunctionFor(arr.elem_type) << "(" << arr.name << ", " << count << ", "
        << seed_offset << "u);\n";
    seed_offset += 1000;
  }
  // Output-only arrays: separate seq/par buffers so nothing but the actual
  // schedule difference can explain a divergence.
  for (const ArrayParamInfo& arr : gen.arrays) {
    if (!arr.is_output || arr.is_input) continue;
    std::string count = CountExpr(arr.dims);
    out << "  " << CTypeName(arr.elem_type) << "* " << arr.name << "_seq = calloc(" << count
        << ", sizeof(" << CTypeName(arr.elem_type) << "));\n";
    out << "  " << CTypeName(arr.elem_type) << "* " << arr.name << "_par = calloc(" << count
        << ", sizeof(" << CTypeName(arr.elem_type) << "));\n";
  }
  out << "\n";

  auto call_args = [&](const std::string& variant_suffix) {
    std::string args;
    bool first = true;
    for (const ArrayParamInfo& arr : gen.arrays) {
      if (!first) args += ", ";
      first = false;
      args += arr.is_output && !arr.is_input ? (arr.name + variant_suffix) : arr.name;
    }
    for (const std::string& dim : gen.dim_params) {
      if (!first) args += ", ";
      first = false;
      args += dim;
    }
    return args;
  };

  if (gen.is_reduction) {
    out << "  " << gen.return_c_type << " result_seq = " << gen.seq_function_name << "("
        << call_args("") << ");\n";
    out << "  " << gen.return_c_type << " result_par = " << gen.par_function_name << "("
        << call_args("") << ");\n\n";
    // Epsilon-relative tolerance: OpenMP recombines per-thread partial sums
    // in a different order than the sequential accumulation, so rounding may
    // differ in the last few bits even though the result is mathematically
    // correct (specs/06 section 4) -- this is expected, not a bug.
    out << "  double a = (double)result_seq, b = (double)result_par;\n"
           "  double diff = fabs(a - b);\n"
           "  double denom = fabs(a) > 1e-9 ? fabs(a) : 1e-9;\n"
           "  double rel = diff / denom;\n"
           "  if (rel < 1e-4) {\n"
           "    printf(\"CERTIFICATE: "
        << kernel.name
        << " STATICALLY SAFE, EMPIRICALLY CORROBORATED\\n\");\n"
           "    printf(\"  (seq=%.6f, par=%.6f, relative diff=%.2e, within reduction "
           "tolerance)\\n\", a, b, rel);\n"
           "    return 0;\n"
           "  } else {\n"
           "    printf(\"CERTIFICATE MISMATCH: "
        << kernel.name
        << " seq and par results disagree beyond tolerance: seq=%.6f par=%.6f "
           "relative diff=%.2e\\n\", a, b, rel);\n"
           "    return 1;\n"
           "  }\n";
  } else {
    out << "  " << gen.seq_function_name << "(" << call_args("_seq") << ");\n";
    out << "  " << gen.par_function_name << "(" << call_args("_par") << ");\n\n";
    // Exact equality: every output element of a map-style kernel is computed
    // independently of every other, so parallelizing the loop does not
    // reorder that element's own arithmetic -- the two variants must be
    // bit-identical (specs/06 section 4).
    const ArrayParamInfo* out_array = nullptr;
    for (const ArrayParamInfo& arr : gen.arrays) {
      if (arr.is_output && !arr.is_input) out_array = &arr;
    }
    std::string count = out_array ? CountExpr(out_array->dims) : "0";
    out << "  size_t total = " << count << ";\n"
        << "  size_t mismatch_at = (size_t)-1;\n"
        << "  for (size_t idx = 0; idx < total; idx++) {\n"
        << "    if (" << out_array->name << "_seq[idx] != " << out_array->name
        << "_par[idx]) { mismatch_at = idx; break; }\n"
        << "  }\n"
        << "  if (mismatch_at == (size_t)-1) {\n"
        << "    printf(\"CERTIFICATE: " << kernel.name
        << " STATICALLY SAFE, EMPIRICALLY CORROBORATED\\n\");\n"
        << "    printf(\"  (%zu output elements compared exactly, all matched)\\n\", "
           "total);\n"
        << "    return 0;\n"
        << "  } else {\n"
        << "    printf(\"CERTIFICATE MISMATCH: " << kernel.name
        << " diverges at index %zu: seq=%f par=%f\\n\", mismatch_at, (double)"
        << out_array->name << "_seq[mismatch_at], (double)" << out_array->name
        << "_par[mismatch_at]);\n"
        << "    return 1;\n"
        << "  }\n";
  }
  out << "}\n";
  return out.str();
}

std::string BuildSmokeHarness(const KernelDecl& kernel, const CodegenResult& gen) {
  std::ostringstream out;
  out << Preamble(gen);
  out << "int main(void) {\n";
  for (size_t i = 0; i < gen.dim_params.size(); ++i) {
    out << "  int " << gen.dim_params[i] << " = " << DimTestValue(i) << ";\n";
  }
  out << "\n";
  unsigned seed_offset = 0;
  for (const ArrayParamInfo& arr : gen.arrays) {
    std::string count = CountExpr(arr.dims);
    if (arr.is_input) {
      out << "  " << CTypeName(arr.elem_type) << "* " << arr.name << " = malloc((" << count
          << ") * sizeof(" << CTypeName(arr.elem_type) << "));\n";
      out << "  " << FillFunctionFor(arr.elem_type) << "(" << arr.name << ", " << count << ", "
          << seed_offset << "u);\n";
      seed_offset += 1000;
    } else {
      out << "  " << CTypeName(arr.elem_type) << "* " << arr.name << " = calloc(" << count
          << ", sizeof(" << CTypeName(arr.elem_type) << "));\n";
    }
  }
  out << "\n";
  std::string args;
  bool first = true;
  for (const ArrayParamInfo& arr : gen.arrays) {
    if (!first) args += ", ";
    first = false;
    args += arr.name;
  }
  for (const std::string& dim : gen.dim_params) {
    if (!first) args += ", ";
    first = false;
    args += dim;
  }
  if (gen.return_is_array) {
    out << "  " << gen.seq_function_name << "(" << args << ");\n";
  } else {
    out << "  " << gen.return_c_type << " result = " << gen.seq_function_name << "(" << args
        << ");\n"
        << "  (void)result;\n";
  }
  out << "  printf(\"SMOKE TEST: " << kernel.name
      << " sequential variant ran to completion\\n\");\n"
      << "  return 0;\n"
      << "}\n";
  return out.str();
}

}  // namespace

HarnessResult GenerateHarness(const KernelDecl& kernel, const PIRKernel& pir,
                              const Schedule& schedule, const CodegenResult& gen) {
  (void)pir;
  HarnessResult result;
  if (schedule.kind == ScheduleKind::SEQUENTIAL_ONLY) {
    result.attempts_certificate = false;
    result.c_source = BuildSmokeHarness(kernel, gen);
  } else {
    result.attempts_certificate = true;
    result.c_source = BuildCertifyingHarness(kernel, schedule, gen);
  }
  return result;
}

}  // namespace parallix
