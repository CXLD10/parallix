// T6 - deliberately malformed: reference to an undeclared identifier (Z).
// Expected Phase 1 behavior: rejected at semantic analysis (specs/02, rule 4),
// with a diagnostic naming the undeclared identifier. Never reaches dependence
// analysis.
kernel undeclared_ref(A: f32[N]) -> B: f32[N] {
    auto i in 0..N {
        B[i] = A[i] + Z[i]
    }
}
