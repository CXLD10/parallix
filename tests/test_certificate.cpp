// Machine-readable certificates and the independent checker, including
// detection of tampering.

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "parallix/certcheck.h"
#include "parallix/certificate.h"
#include "parallix/json.h"
#include "parallix/sha256.h"

using namespace parallix;

// A directory unique to the running test, so tests can run in parallel.
inline std::string UniqueTestDir() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  return std::string(info ? info->test_suite_name() : "x") + "_" + (info ? info->name() : "x");
}

namespace {

std::string Scratch() {
  std::filesystem::path dir = std::filesystem::path("build") / "test_generated" / UniqueTestDir();
  std::filesystem::create_directories(dir);
  return dir.string();
}

struct Issued {
  std::string source, json;
};

Analysis Analyze(const std::string& text) {
  std::string path = Scratch() + "/cert_kernel.plx";
  { std::ofstream(path) << text; }
  ParsedSource parsed;
  std::string error;
  EXPECT_TRUE(ParseSourceFile(path, &parsed, &error)) << error;
  Analysis a;
  EXPECT_TRUE(AnalyzeKernelDecl(std::move(parsed.program.kernels[0]), &a, &error)) << error;
  return a;
}

const char* kFibSrc = "kernel fib(dp: i32[N]) -> dp: i32[N] {\n  auto i in 2..N {\n    dp[i] = dp[i-1] + dp[i-2]\n  }\n}\n";
const char* kAddSrc = "kernel vadd(A: f32[N], B: f32[N]) -> C: f32[N] {\n  auto i in 0..N {\n    C[i] = A[i] + B[i]\n  }\n}\n";

Issued IssueRefuted() {
  Analysis a = Analyze(kFibSrc);
  RefutationOptions ro;
  ro.work_dir = Scratch();
  RefutationReport rr = RunRefutation(a, ro);
  CertificateInput in;
  in.source_path = "fib.plx";
  in.source_text = kFibSrc;
  in.analysis = &a;
  in.refutation = &rr;
  return {kFibSrc, BuildCertificateJson(in)};
}

Issued IssueSafe() {
  Analysis a = Analyze(kAddSrc);
  ValidationOptions vo;
  vo.tsan = false;
  vo.work_dir = Scratch();
  ValidationReport vr = RunValidation(a, vo);
  CertificateInput in;
  in.source_path = "vadd.plx";
  in.source_text = kAddSrc;
  in.analysis = &a;
  in.validation = &vr;
  return {kAddSrc, BuildCertificateJson(in)};
}

std::string Replace(std::string s, const std::string& from, const std::string& to) {
  size_t p = s.find(from);
  EXPECT_NE(p, std::string::npos) << "tamper target not found: " << from;
  if (p != std::string::npos) s.replace(p, from.size(), to);
  return s;
}

std::string Failures(const CertCheckResult& r) {
  std::string s;
  for (const auto& f : r.failures) s += f + "\n";
  return s;
}

}  // namespace

TEST(Sha256, KnownVectors) {
  EXPECT_EQ(Sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Json, EscapesAndNests) {
  Json o = Json::Object();
  o.Set("a", "x\"y\n").Set("n", 3).Set("list", Json::Array().Add(1).Add(true));
  EXPECT_EQ(o.Dump(), "{\"a\":\"x\\\"y\\n\",\"n\":3,\"list\":[1,true]}");
}

TEST(Certificate, RefutedCertificateIsAcceptedByTheChecker) {
  Issued c = IssueRefuted();
  CertCheckResult r = CheckCertificate(c.json, &c.source);
  EXPECT_TRUE(r.ok) << Failures(r);
  EXPECT_NE(c.json.find("\"status\": \"REFUTED\""), std::string::npos);
  EXPECT_FALSE(r.passed.empty());
}

TEST(Certificate, SafeCertificateIsAcceptedByTheChecker) {
  Issued c = IssueSafe();
  CertCheckResult r = CheckCertificate(c.json, &c.source);
  EXPECT_TRUE(r.ok) << Failures(r);
  EXPECT_NE(c.json.find("\"status\": \"CORROBORATED\""), std::string::npos);
}

TEST(Certificate, RecordsSeedsSizesThreadsAndToolchain) {
  Issued c = IssueSafe();
  for (const char* key : {"\"seeds\"", "\"sizes\"", "\"threads\"", "\"sha256\"", "\"toolchain\"",
                          "\"repetitions\"", "\"permutations\"", "\"schedule\"", "\"machine\""}) {
    EXPECT_NE(c.json.find(key), std::string::npos) << key;
  }
}

TEST(CertificateTamper, EditedSourceTextIsDetected) {
  Issued c = IssueRefuted();
  std::string t = Replace(c.json, "dp[i-1]", "dp[i-9]");
  CertCheckResult r = CheckCertificate(t, nullptr);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(Failures(r).find("source hash mismatch"), std::string::npos) << Failures(r);
}

TEST(CertificateTamper, SourceFileThatDiffersFromTheCertificateIsDetected) {
  Issued c = IssueRefuted();
  std::string other = c.source + "// edited\n";
  CertCheckResult r = CheckCertificate(c.json, &other);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(Failures(r).find("on-disk source file differs"), std::string::npos);
}

TEST(CertificateTamper, WitnessIterationMovedOutOfConflictIsDetected) {
  Issued c = IssueRefuted();
  // The first witness names iterations i=2 (write) and i=3 (read). Moving the
  // reader to i=5 makes its subscript dp[i-1] evaluate to 4, not 2.
  size_t second = c.json.find("\"second\"");
  ASSERT_NE(second, std::string::npos);
  size_t at = c.json.find("\"i\": 3", second);
  ASSERT_NE(at, std::string::npos);
  std::string t = c.json;
  t.replace(at, 6, "\"i\": 5");
  CertCheckResult r = CheckCertificate(t, &c.source);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(Failures(r).find("do not demonstrate a conflict"), std::string::npos) << Failures(r);
}

TEST(CertificateTamper, IterationOutsideTheLoopBoundsIsDetected) {
  Issued c = IssueRefuted();
  size_t first = c.json.find("\"first\"");
  size_t at = c.json.find("\"i\": 2", first);
  ASSERT_NE(at, std::string::npos);
  std::string t = c.json;
  t.replace(at, 6, "\"i\": 1");  // loop starts at 2
  EXPECT_FALSE(CheckCertificate(t, &c.source).ok);
}

TEST(CertificateTamper, ForgedStatusIsDetected) {
  Issued c = IssueRefuted();
  EXPECT_FALSE(CheckCertificate(Replace(c.json, "\"status\": \"REFUTED\"", "\"status\": \"CORROBORATED\""), &c.source).ok);
}

TEST(CertificateTamper, HidingAMismatchInTheRunCountsIsDetected) {
  Issued c = IssueSafe();
  // Claim a mismatch in one cell without updating the totals.
  std::string t = Replace(c.json, "\"mismatches\": 0", "\"mismatches\": 1");
  CertCheckResult r = CheckCertificate(t, &c.source);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(Failures(r).find("totals"), std::string::npos) << Failures(r);
}

TEST(CertificateTamper, FlippingADependenceVerdictToSafeIsDetected) {
  Issued c = IssueRefuted();
  std::string t = Replace(c.json, "\"verdict\": \"UNSAFE\"", "\"verdict\": \"SAFE\"");
  t = Replace(t, "\"status\": \"REFUTED\"", "\"status\": \"CORROBORATED\"");
  CertCheckResult r = CheckCertificate(t, &c.source);
  EXPECT_FALSE(r.ok);  // the recorded pairs still contain a dependent pair
}

TEST(CertificateTamper, GarbageIsRejectedNotCrashed) {
  EXPECT_FALSE(CheckCertificate("{not json", nullptr).ok);
  EXPECT_FALSE(CheckCertificate("{\"format\":\"other\"}", nullptr).ok);
}

TEST(Certificate, InPlaceKernelWithSameIterationPairIsAcceptedAndForgeryIsNot) {
  const char* src = "kernel bump(A: f32[N], B: f32[N]) -> B: f32[N] {\n  auto i in 0..N {\n    B[i] = B[i] + A[i]\n  }\n}\n";
  Analysis a = Analyze(src);
  ASSERT_EQ(a.report.verdict, Verdict::SAFE);
  ValidationOptions vo;
  vo.tsan = false;
  vo.work_dir = Scratch();
  ValidationReport vr = RunValidation(a, vo);
  CertificateInput in;
  in.source_path = "bump.plx";
  in.source_text = src;
  in.analysis = &a;
  in.validation = &vr;
  std::string json = BuildCertificateJson(in);
  EXPECT_NE(json.find("same-iteration"), std::string::npos);
  EXPECT_TRUE(CheckCertificate(json, nullptr).ok);
  // Forge the rule on a pair whose offsets differ: not the same iteration.
  std::string forged = Replace(json, "\"const\": 0", "\"const\": 1");
  forged = Replace(forged, "\"sha256\"", "\"sha256\"");  // hash covers only the source text, untouched
  EXPECT_FALSE(CheckCertificate(forged, nullptr).ok);
}
