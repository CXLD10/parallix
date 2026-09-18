// T4 - 2D five-point stencil, different arrays (A read, B written).
// Expected Phase 1 verdict: SAFE (no dependence - different arrays; see specs/04).
kernel stencil5(A: f32[N,M]) -> B: f32[N,M] {
    parallel i in 1..N-1 {
        vectorize j in 1..M-1 {
            B[i,j] = A[i-1,j] + A[i+1,j] + A[i,j-1] + A[i,j+1] + A[i,j]
        }
    }
}
