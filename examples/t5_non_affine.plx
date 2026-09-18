// T5 - deliberately malformed: non-affine subscript (i * i).
// Expected Phase 1 behavior: rejected at semantic analysis (specs/02, rule 1),
// with a diagnostic naming the offending subscript. Never reaches dependence
// analysis.
kernel non_affine(A: f32[N]) -> B: f32[N] {
    auto i in 0..N {
        B[i] = A[i * i]
    }
}
