// T2 - self-recurrence: writes and reads the same array one index apart.
// Expected verdict: UNSAFE, loop-carried RAW, distance 1.
kernel prefix_like(A: f32[N], B: f32[N]) -> A: f32[N] {
    auto i in 1..N {
        A[i] = A[i-1] + B[i]
    }
}
