// Demo B - climbing stairs / Fibonacci DP: dp[i] = dp[i-1] + dp[i-2].
// dp is read and written in place, so dp[0] and dp[1] are the base cases
// supplied by the caller (set both to 1 for the classic stair-climbing count).
// Integer arithmetic wraps on overflow.
// Expected verdict: UNSAFE (loop-carried RAW at distances 1 and 2).
kernel climbing_stairs(dp: i32[N]) -> dp: i32[N] {
    auto i in 2..N {
        dp[i] = dp[i-1] + dp[i-2]
    }
}
