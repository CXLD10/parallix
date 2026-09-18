# Parallix

A from-scratch C++17 compiler for a small affine array DSL, built for the Compiler
Design Laboratory course (BCSE307P, VIT Vellore).

**Current scope: Phase 1 prototype.** Lexer, parser, semantic analysis, and a
hand-written dependence-analysis engine that classifies six fixture kernels as safe,
unsafe, or safe-as-reduction. No scheduling, code generation, or MLIR/LLVM yet.

## Build

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

## Project layout

```
src/         Implementation (parallix_core library + the CLI executable)
include/     Public headers for parallix_core
tests/       GoogleTest unit tests, one file per module
examples/    Six fixture .plx kernels, T1-T6, used by every milestone's tests
```
