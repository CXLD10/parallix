# Parallix

A from-scratch C++17 compiler for a small affine array DSL. Parallix decides, per
kernel, whether its loops can safely run in parallel, generates sequential and OpenMP
C code accordingly, and then empirically verifies its own static verdict: the compiler
certifies its dependence analysis instead of only asserting it.

## Status

| Phase | Scope | State |
|---|---|---|
| 1 | Lexer, parser, semantic analysis, hand-written GCD / Banerjee-style dependence analysis with SAFE / UNSAFE / safe-as-reduction verdicts | Complete |
| 2 | Rule-based schedule generation, sequential/OpenMP C code generation, compile-and-run driver, sequential-vs-parallel certification harness | Complete |

All 44 unit tests pass. Out of scope so far: MLIR/LLVM backends, autotuned scheduling,
and counterexample generation for UNSAFE kernels.

## Pipeline

```
source (.plx) -> lexer -> parser -> semantic analysis -> dependence analysis
              -> schedule -> C / OpenMP codegen -> compile & run -> certificate
```

Every rejection (semantic error or UNSAFE verdict) carries a human-readable reason.
For SAFE kernels the generated test harness runs the sequential and parallel versions
and reports `STATICALLY SAFE, EMPIRICALLY CORROBORATED` when the outputs agree.

## Build

Requires a C++17 compiler, CMake, and a C compiler (`cc`) on `PATH` for the
compile-and-run stage. GoogleTest is fetched automatically.

```bash
cmake -S . -B build
cmake --build build -j
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

## Run

```bash
./build/src/parallix examples/t1_vector_add.plx
```

Output sections: `TOKENS`, `AST`, `SEMANTIC INFO`, `LOOP ANALYSIS`,
`DEPENDENCE ANALYSIS`, `PARALLELIZATION`, `SCHEDULE`, `GENERATED CODE`, `EXECUTION`,
`CERTIFICATE`. Generated sources are written to `generated/`.

## Example kernels

| Kernel | Expected outcome |
|---|---|
| `t1_vector_add` | SAFE, parallelized and certified |
| `t2_prefix_like` | UNSAFE (loop-carried dependence), sequential only |
| `t3_reduce_sum` | Safe as a reduction, parallelized and certified |
| `t4_stencil5` | SAFE, parallelized and certified |
| `t5_non_affine` | Rejected: non-affine subscript |
| `t6_undeclared` | Rejected: undeclared identifier |

## Project layout

```
src/         Implementation (parallix_core library + the CLI executable)
include/     Public headers for parallix_core
tests/       GoogleTest unit tests, one file per module
examples/    Six fixture .plx kernels, T1-T6
```
