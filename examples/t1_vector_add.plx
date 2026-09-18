// T1 - elementwise, two distinct arrays read, a third written.
// Expected Phase 1 verdict: SAFE (no dependence - different arrays; see specs/04).
kernel vector_add(A: f32[N], B: f32[N]) -> C: f32[N] {
    auto i in 0..N {
        C[i] = A[i] + B[i]
    }
}
