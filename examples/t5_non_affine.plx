// T5 - deliberately malformed: non-affine subscript (i * i).
// Expected behavior: rejected at semantic analysis,
// with a diagnostic naming the offending subscript. Never reaches dependence
// analysis.
kernel non_affine(A: f32[N]) -> B: f32[N] {
    auto i in 0..N {
        B[i] = A[i * i]
    }
}
