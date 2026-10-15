// Demo A - count pairs (i < j) with a[i] + a[j] == target, brute force.
// Triangular inner loop, integer count reduction. Still O(n^2): parallelism
// divides the constant factor, it does not change the complexity.
// Expected verdict: SAFE_AS_REDUCTION.
kernel count_pairs(a: i32[N], target: i32) -> count: i64 {
    reduce(+) i in 0..N {
        auto j in i+1..N {
            count += select(a[i] + a[j] == target, 1, 0)
        }
    }
}
