// T6 - deliberately malformed: reference to an undeclared identifier (Z).
// Expected behavior: rejected at semantic analysis,
// with a diagnostic naming the undeclared identifier. Never reaches dependence
// analysis.
kernel undeclared_ref(A: f32[N]) -> B: f32[N] {
    auto i in 0..N {
        B[i] = A[i] + Z[i]
    }
}
