// T3 - associative reduction into a scalar accumulator.
// Expected verdict: SAFE_AS_REDUCTION.
kernel reduce_sum(A: f32[N]) -> s: f32 {
    reduce(+) i in 0..N {
        s += A[i]
    }
}
