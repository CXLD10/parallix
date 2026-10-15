# Parallix

Parallix is a from-scratch C++17 compiler for a small affine array-loop language. For each
kernel it decides whether the loops can run in parallel, generates sequential and OpenMP C
accordingly, and then **tries to prove its own verdict wrong**:

* a kernel it calls safe is attacked with reversed and randomly permuted iteration orders and
  with the parallel code at many thread counts, seeds and sizes;
* a kernel it calls unsafe gets a concrete counterexample built from the dependence witness;
* both outcomes are written to a JSON certificate that a separate small checker can re-verify.

The dependence engine (GCD test, distance/direction analysis, RAW/WAR/WAW) is hand-written;
no library does the reasoning.

## Prerequisites

* A C++17 compiler (g++ 7 or newer), CMake 3.20+, make
* A C compiler on `PATH` (`cc` or `gcc`): Parallix compiles the C it generates
* OpenMP (ships with gcc as `libgomp`) for real parallel runs
* GoogleTest, only for `make test` (a system copy is used if present, otherwise it is fetched,
  which needs network access)

`./install.sh` checks all of this and prints a fix for anything missing.

## Quickstart

```bash
make                                                    # build (no test dependencies)
./build/src/parallix verify examples/count_pairs.plx    # analyze, attack the verdict, write a certificate
./build/src/parallix verify examples/climbing_stairs.plx --force-parallel   # an unsafe kernel: counterexample
./build/src/parallix check-cert generated/count_pairs.cert.json --source examples/count_pairs.plx
./demo.sh --fast                                        # the guided two-part demo
```

## Commands

| Command | What it does |
|---|---|
| `parallix analyze <file>` | lex, parse, check, dependence analysis, schedule, cost model |
| `parallix explain <file>` | per-loop decision record (rule, evidence, rejected alternatives); also writes an HTML report |
| `parallix run <file> --size N` | compile and run the chosen schedule; reports the result and time |
| `parallix verify <file>` | adversarial validation for safe kernels, counterexample for unsafe ones; writes `<kernel>.cert.json` |
| `parallix bench <file> --sizes ... --threads ...` | median of at least 5 runs, table + CSV + SVG chart |
| `parallix check-cert <cert.json>` | independent re-check of a certificate |
| `parallix demo` | runs `demo.sh` |

Useful options: `--out DIR` (default `generated`), `--scalar NAME=VALUE`, `--no-tsan`,
`--force-parallel`, `--sizes/--seeds/--threads/--reps/--perms`.

## The language in one example

```
kernel count_pairs(a: i32[N], target: i32) -> count: i64 {
    reduce(+) i in 0..N {
        auto j in i+1..N {
            count += select(a[i] + a[j] == target, 1, 0)
        }
    }
}
```

Types `f32 f64 i32 i64`; arrays of any rank; affine loop bounds (triangular bounds such as
`j in i+1..N` work); comparisons (`== != < <= > >=`), `select(cond, a, b)`, `min`, `max` as
value expressions; `reduce(+|*|min|max)` loops. Comparisons and `select` are rejected inside
subscripts and loop bounds (`E_COMPARISON_IN_SUBSCRIPT` and friends) so subscripts stay affine
and the dependence analysis stays exact. Integer arithmetic wraps on overflow.

## What a run looks like

Real output, captured on a 20-thread i7-13650HX (see `results/phase3/`).

Safe kernel (`parallix verify examples/count_pairs.plx`):

```
VALIDATION: order independence -- reversed + 3 seeded permutations of loop 'i' x 3 sizes (37,101,257) x 3 seeds (1,2,3) = 36 runs, 0 mismatches
VALIDATION: parallel -- threads 1,2,4,8,20 (machine max 20) x 20 repetitions x 3 sizes x 3 seeds = 900 runs, 0 mismatches
VALIDATION: ThreadSanitizer -- skipped: ThreadSanitizer cannot judge OpenMP programs on this toolchain: ...
CERTIFICATE: count_pairs STATICALLY SAFE, EMPIRICALLY CORROBORATED
```

Unsafe kernel (`parallix verify examples/climbing_stairs.plx --force-parallel`):

```
WITNESS: loop-carried RAW on 'dp': iteration (i=2) writes dp[2] via dp[i](write); iteration (i=3) reads the same element via dp[i - 1](read). ...
COUNTEREXAMPLE (deterministic): size 12, seed 1: running loop 'i' in reversed order ... changes dp[3] -- first diverging index 3: sequential=167, reversed=136
FORCED PARALLEL (nondeterministic; the reversed-order result above is the proof): 200 of 200 runs at 20 threads differed from sequential
CERTIFICATE: REFUTED -- climbing_stairs is not safe to parallelize: RAW dependence on 'dp' ...
```

Benchmark (`parallix bench examples/count_pairs.plx --sizes 160000 --threads 1,2,4,8,16,20 --scalar target=100`):

```
size        variant threads      median (s)     speedup  efficiency  check
160000      seq     1              4.160795       1.00x        100%  ok
160000      par     4              1.121624       3.71x         93%  ok
160000      par     8              0.747237       5.57x         70%  ok
160000      par     16             0.512985       8.11x         51%  ok
160000      par     20             0.472602       8.80x         44%  ok
```

The algorithm is still O(n^2). Parallelization divides the constant factor; it does not change
the complexity. Scaling flattens past the machine's fast cores (this CPU mixes performance and
efficiency cores). Full tables, the CSV and the SVG chart are in `results/phase3/`.

## The demo

`./demo.sh` (about 3 minutes paced, 40 seconds with `--fast`; `--step` waits for Enter for live
use; `--part a|b` runs one part).

* **Part A** counts pairs with `a[i] + a[j] == target`: triangular loop, integer reduction. Expected:
  SAFE_AS_REDUCTION, a `reduction(+:count)` schedule, validation passes, the certificate checks, and
  a measured speedup.
* **Part B** is `dp[i] = dp[i-1] + dp[i-2]`: RAW at distances 1 and 2, no parallel code, a
  reversed-order counterexample, forced-parallel disagreement, and `CERTIFICATE: REFUTED`.

## Installing

```bash
./install.sh            # checks prerequisites, builds, installs to /usr/local or ~/.local
make test               # build and run the test suite (123 tests)
make sanitize           # the same suite under AddressSanitizer + UBSan
```

Docker (Ubuntu 22.04): `docker build -t parallix .` then `docker run --rm -it parallix`.

## Troubleshooting

* **OpenMP is missing.** If the C compiler rejects `-fopenmp`, Parallix retries without it. Everything
  still runs, but parallel variants execute on one thread, and the certificate line says so
  ("built without OpenMP"). Fix: install your compiler's OpenMP runtime (`libgomp1` on
  Ubuntu/Debian; on macOS use Homebrew gcc and set `CC`).
* **No `cc`.** Parallix tries `cc`, then `gcc`. Install either (`sudo apt install gcc`).
* **Single-core machine.** The cost model declines to parallelize ("only 1 logical CPU") and
  validation covers one thread; speedups will be about 1.0x. This is expected.
* **ThreadSanitizer is skipped.** It is run only if a race-free OpenMP control program passes under it.
  Uninstrumented OpenMP runtimes and some kernels (notably WSL2 with GCC 11) make TSan unusable; the
  skip and its reason are printed and recorded in the certificate.
* **`make test` cannot fetch GoogleTest.** Install it (`sudo apt install libgtest-dev`) or build with
  network access.

## Limits

A certificate is strong evidence plus a checkable witness, not a formal proof for all inputs. There is
one backend (C + OpenMP). Only affine loops are supported, and only the outermost loop is parallelized.
The dependence engine is conservative where it cannot compute a distance. See
`results/phase3/REPORT.md` for the full list.

## Layout

```
src/        compiler, harness generator, verifier, checker, CLI
include/    public headers
tests/      GoogleTest unit and integration tests
examples/   six fixtures (t1-t6) and the two demo kernels
results/    captured outputs of real runs
demo.sh  install.sh  Makefile  Dockerfile
```
