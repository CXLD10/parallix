#!/usr/bin/env bash
# Checks prerequisites, builds parallix, and installs it.
#   ./install.sh                 install to /usr/local (or ~/.local if that is not writable)
#   PREFIX=/opt/parallix ./install.sh
#   ./install.sh --test          also build and run the test suite
set -u

RUN_TESTS=0
[ "${1:-}" = "--test" ] && RUN_TESTS=1

FAILED=0
ok()   { printf '  [ok]   %s\n' "$1"; }
bad()  { printf '  [FAIL] %s\n         fix: %s\n' "$1" "$2"; FAILED=1; }
warn() { printf '  [warn] %s\n         note: %s\n' "$1" "$2"; }

echo "Checking prerequisites..."

if command -v g++ >/dev/null 2>&1; then
  ver=$(g++ -dumpversion | cut -d. -f1)
  if [ "$ver" -ge 7 ] 2>/dev/null; then ok "g++ $(g++ -dumpversion) (C++17)"; else
    bad "g++ $ver is too old for C++17" "install g++ 7 or newer (Ubuntu/Debian: sudo apt install g++)"; fi
else
  bad "g++ not found" "Ubuntu/Debian: sudo apt install g++   Fedora: sudo dnf install gcc-c++   macOS: brew install gcc"
fi

if command -v cmake >/dev/null 2>&1; then ok "cmake $(cmake --version | head -1 | awk '{print $3}')"; else
  bad "cmake not found" "Ubuntu/Debian: sudo apt install cmake   Fedora: sudo dnf install cmake   macOS: brew install cmake"; fi

if command -v make >/dev/null 2>&1; then ok "make"; else
  bad "make not found" "Ubuntu/Debian: sudo apt install make"; fi

CC_BIN=$(command -v cc || command -v gcc || true)
if [ -n "$CC_BIN" ]; then ok "C compiler: $CC_BIN"; else
  bad "no C compiler (cc or gcc) on PATH" "Ubuntu/Debian: sudo apt install gcc   (parallix compiles the C it generates)"; fi

if [ -n "$CC_BIN" ]; then
  tmp=$(mktemp -d)
  printf '#include <omp.h>\nint main(void){return omp_get_max_threads()>0?0:1;}\n' > "$tmp/t.c"
  if "$CC_BIN" -fopenmp "$tmp/t.c" -o "$tmp/t" 2>/dev/null && "$tmp/t"; then
    ok "OpenMP works ($(nproc 2>/dev/null || echo '?') logical CPUs)"
  else
    warn "OpenMP is not available for $CC_BIN" \
         "parallix still works but runs parallel variants on 1 thread. Ubuntu/Debian: sudo apt install libgomp1 (ships with gcc); macOS: brew install gcc and run with CC=gcc-14"
  fi
  rm -rf "$tmp"
  if [ "$(nproc 2>/dev/null || echo 2)" -lt 2 ]; then
    warn "only 1 logical CPU detected" "certificates will cover 1 thread and speedups will be about 1.0x"
  fi
fi

if [ "$FAILED" -ne 0 ]; then
  echo; echo "Prerequisites missing: fix the items marked FAIL and re-run."; exit 1
fi

PREFIX="${PREFIX:-}"
if [ -z "$PREFIX" ]; then
  if [ -w /usr/local ] || [ "$(id -u)" -eq 0 ]; then PREFIX=/usr/local; else PREFIX="$HOME/.local"; fi
fi

echo; echo "Building..."
make all || exit 1
if [ "$RUN_TESTS" -eq 1 ]; then echo; echo "Testing..."; make test || exit 1; fi
echo; echo "Installing to $PREFIX..."
make install PREFIX="$PREFIX" || { echo "install failed; try: PREFIX=\$HOME/.local ./install.sh"; exit 1; }
echo
echo "Done. Run: $PREFIX/bin/parallix --help"
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) echo "Add $PREFIX/bin to your PATH to run it as 'parallix'." ;; esac
echo "Guided demo: ./demo.sh   (or ./demo.sh --fast)"
