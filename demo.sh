#!/usr/bin/env bash
# Parallix guided demo: two kernels, one parallelizable and one not.
#
#   ./demo.sh            paced, readable (about 4-5 minutes)
#   ./demo.sh --fast     no pauses, smaller benchmark (under 2 minutes)
#   ./demo.sh --step     wait for Enter between steps (for live presenting)
#   ./demo.sh --part a   only Part A (or: --part b)
#
# Every number printed is produced by a real run on this machine.
set -u

FAST=0; STEP=0; PART=all
while [ $# -gt 0 ]; do
  case "$1" in
    --fast) FAST=1 ;;
    --step) STEP=1 ;;
    --part) PART="${2:-all}"; shift ;;
    -h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option $1 (try --help)"; exit 1 ;;
  esac
  shift
done

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -n "${PARALLIX:-}" ]; then BIN="$PARALLIX"
elif [ -x "$HERE/build/src/parallix" ]; then BIN="$HERE/build/src/parallix"
elif [ -x "$HERE/../../bin/parallix" ]; then BIN="$HERE/../../bin/parallix"   # installed layout: <prefix>/share/parallix/demo.sh
elif command -v parallix >/dev/null 2>&1; then BIN="$(command -v parallix)"
else echo "parallix is not built. Run: make   (or ./install.sh)"; exit 1; fi
EX="$HERE/examples"; [ -d "$EX" ] || EX="$HERE/../examples"
OUT="${DEMO_OUT:-$PWD/demo-out}"
mkdir -p "$OUT"

if [ -t 1 ]; then B=$'\033[1m'; D=$'\033[2m'; C=$'\033[36m'; G=$'\033[32m'; N=$'\033[0m'; else B=; D=; C=; G=; N=; fi

pause() {  # pause SECONDS
  [ "$FAST" -eq 1 ] && return
  if [ "$STEP" -eq 1 ] && [ -t 0 ]; then printf '%s   [Enter to continue]%s' "$D" "$N"; read -r _; echo
  else sleep "$1"; fi
}
say()   { printf '\n%s%b%s\n' "$C" "$1" "$N"; pause "${2:-3}"; }
head1() { printf '\n%s%s%s\n%s\n' "$B" "$1" "$N" "$(printf '=%.0s' $(seq 1 ${#1}))"; pause 2; }
pretty(){ local t="$*"; t="${t//$BIN/parallix}"; t="${t//$OUT/demo-out}"; t="${t//$EX/examples}"; printf '%s' "$t"; }
run()   { printf '%s$ %s%s\n' "$G" "$(pretty "$@")" "$N"; pause 1; "$@"; local rc=$?; pause 3; return $rc; }
show()  { printf '%s$ cat examples/%s%s\n' "$G" "$(basename "$1")" "$N"; sed 's/^/    /' "$1"; pause 4; }
KEYRE='^(LOOP ANALYSIS|DEPENDENCE ANALYSIS|PARALLELIZATION|SCHEDULE|COST MODEL)'

start=$(date +%s)
printf '%sParallix%s -- a compiler that checks its own parallelization verdicts\n' "$B" "$N"
say "Two kernels. One can be parallelized and one cannot. Parallix has to say which, say why,\nand then try to prove itself wrong." 4

if [ "$PART" = all ] || [ "$PART" = a ]; then
  head1 "PART A: a parallelizable O(n^2) problem"
  say "Count the pairs (i < j) with a[i] + a[j] == target, by brute force. The inner loop is\ntriangular (j starts at i+1) and the result is an integer count." 4
  show "$EX/count_pairs.plx"

  say "Step 1 -- analysis. Parallix compares every pair of array references with a GCD test and\ndistance analysis it implements itself. Both references read 'a', so nothing conflicts; the only\ncarried dependence is the count accumulator, which is a licensed reduction." 5
  printf '%s$ parallix analyze examples/count_pairs.plx --size 160000%s\n' "$G" "$N"
  "$BIN" analyze "$EX/count_pairs.plx" --size 160000 | grep -E "$KEYRE" | cut -c1-230; pause 4

  say "Step 2 -- why. Each loop gets a rule, the evidence, and the alternatives that were rejected." 3
  run "$BIN" explain "$EX/count_pairs.plx" --size 160000 --out "$OUT"

  say "Step 3 -- try to break it. Parallix runs the sequential code, the loop in reversed order and\nin seeded random orders, then the parallel code on 1, 2, 4, 8 and all threads, 20 times each,\nacross several seeds and sizes that are not multiples of the thread count. If the loop were really\norder-independent, nothing should ever differ." 6
  run "$BIN" verify "$EX/count_pairs.plx" --out "$OUT" --cert "$OUT/count_pairs.cert.json"

  say "Step 4 -- the certificate is a file, and a separate small checker re-verifies it without\ntrusting the compiler: it recomputes the source hash and re-derives why the loop is safe." 4
  run "$BIN" check-cert "$OUT/count_pairs.cert.json" --source "$EX/count_pairs.plx"

  say "Step 5 -- is it actually faster? Median of 5 runs, sequential vs parallel, measured now." 3
  say "Be clear about what this is: the algorithm is still O(n^2). Parallelization divides the\nconstant factor across cores; it does not change the complexity." 5
  if [ "$FAST" -eq 1 ]; then BENCH_ARGS=(--sizes 160000 --threads 4,20); else BENCH_ARGS=(--sizes 160000 --threads 1,2,4,8,16,20); fi
  run "$BIN" bench "$EX/count_pairs.plx" "${BENCH_ARGS[@]}" --scalar target=100 --out "$OUT"
  say "The speedup curve flattens past the number of fast cores: that is the machine, not the compiler." 4
fi

if [ "$PART" = all ] || [ "$PART" = b ]; then
  head1 "PART B: a problem that must NOT be parallelized"
  say "Climbing stairs: dp[i] = dp[i-1] + dp[i-2]. Each step needs the two before it." 3
  show "$EX/climbing_stairs.plx"

  say "Step 1 -- analysis. The write dp[i] and the reads dp[i-1], dp[i-2] hit the same elements in\nDIFFERENT iterations: a loop-carried read-after-write at distances 1 and 2." 4
  printf '%s$ parallix analyze examples/climbing_stairs.plx%s\n' "$G" "$N"
  "$BIN" analyze "$EX/climbing_stairs.plx" | grep -E "$KEYRE" | cut -c1-330; pause 4
  say "No parallel code is generated. But a verdict is just a claim -- so Parallix tries to produce\na concrete counterexample from the two conflicting iterations it found." 5

  say "Step 2 -- counterexample. First, deterministically: run the same loop in reversed order and\nshow where the answer changes. Second, for illustration only, force the parallel version anyway and\ncount how often it disagrees with sequential. That second result is nondeterministic and is not the\nproof; the reversed-order divergence is." 7
  run "$BIN" verify "$EX/climbing_stairs.plx" --force-parallel --out "$OUT" --cert "$OUT/climbing_stairs.cert.json"

  say "Step 3 -- the refutation is also a checkable certificate: the checker evaluates the access\nfunctions at the two named iterations and confirms they touch the same element." 4
  run "$BIN" check-cert "$OUT/climbing_stairs.cert.json" --source "$EX/climbing_stairs.plx"
fi

end=$(date +%s)
printf '\n%sDone in %ss.%s Certificates, reports and the benchmark chart are in %s\n' "$B" "$((end-start))" "$N" "$OUT"
echo "Limits, stated plainly: a certificate is strong evidence plus a checkable witness, not a formal"
echo "proof for all inputs; there is one backend (C + OpenMP); only affine loops are supported."
