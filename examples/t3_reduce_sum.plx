// T3 - associative reduction into a scalar accumulator.
// Expected Phase 1 verdict: SAFE_AS_REDUCTION (see specs/04, step 5).
kernel reduce_sum(A: f32[N]) -> s: f32 {
    reduce(+) i in 0..N {
        s += A[i]
    }
}
