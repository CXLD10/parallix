#include "parallix/certcheck.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>

#include "parallix/sha256.h"

namespace parallix {
namespace {

// ---- minimal JSON reader ---------------------------------------------------
struct JV {
  char t = 'n';  // n null, b bool, d number, s string, a array, o object
  std::string s;
  std::vector<JV> a;
  std::vector<std::pair<std::string, JV>> o;
  const JV& operator[](const std::string& k) const {
    static const JV none;
    for (const auto& kv : o) if (kv.first == k) return kv.second;
    return none;
  }
  const JV& operator[](size_t i) const { return a.at(i); }
  long long Int() const { return std::strtoll(s.c_str(), nullptr, 10); }
};

class Reader {
 public:
  explicit Reader(const std::string& text) : s_(text) {}
  JV Parse() { JV v = Value(); Ws(); if (p_ != s_.size()) throw std::runtime_error("trailing data"); return v; }
 private:
  const std::string& s_;
  size_t p_ = 0;
  void Ws() { while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_; }
  char Peek() { Ws(); if (p_ >= s_.size()) throw std::runtime_error("unexpected end"); return s_[p_]; }
  void Expect(char c) { if (Peek() != c) throw std::runtime_error(std::string("expected '") + c + "'"); ++p_; }
  std::string Str() {
    Expect('"');
    std::string r;
    while (p_ < s_.size() && s_[p_] != '"') {
      char c = s_[p_++];
      if (c != '\\') { r += c; continue; }
      char e = s_.at(p_++);
      if (e == 'n') r += '\n'; else if (e == 't') r += '\t'; else if (e == 'r') r += '\r';
      else if (e == 'u') { r += static_cast<char>(std::strtol(s_.substr(p_, 4).c_str(), nullptr, 16)); p_ += 4; }
      else r += e;
    }
    ++p_;
    return r;
  }
  JV Value() {
    JV v;
    char c = Peek();
    if (c == '{') {
      v.t = 'o'; ++p_;
      if (Peek() == '}') { ++p_; return v; }
      for (;;) { std::string k = Str(); Expect(':'); v.o.push_back({k, Value()}); if (Peek() == ',') { ++p_; continue; } Expect('}'); break; }
    } else if (c == '[') {
      v.t = 'a'; ++p_;
      if (Peek() == ']') { ++p_; return v; }
      for (;;) { v.a.push_back(Value()); if (Peek() == ',') { ++p_; continue; } Expect(']'); break; }
    } else if (c == '"') {
      v.t = 's'; v.s = Str();
    } else if (s_.compare(p_, 4, "true") == 0) { v.t = 'b'; v.s = "true"; p_ += 4;
    } else if (s_.compare(p_, 5, "false") == 0) { v.t = 'b'; v.s = "false"; p_ += 5;
    } else if (s_.compare(p_, 4, "null") == 0) { p_ += 4;
    } else {
      v.t = 'd';
      size_t st = p_;
      while (p_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[p_])) || std::strchr("+-.eE", s_[p_]))) ++p_;
      if (st == p_) throw std::runtime_error("bad value");
      v.s = s_.substr(st, p_ - st);
    }
    return v;
  }
};

// ---- tiny integer expression evaluator: + - * unary- ( ) names -------------
class Eval {
 public:
  Eval(const std::string& e, const std::map<std::string, long long>& env) : e_(e), env_(env) {}
  long long Run() { long long v = Sum(); Ws(); if (p_ != e_.size()) throw std::runtime_error("bad expression '" + e_ + "'"); return v; }
 private:
  const std::string& e_;
  const std::map<std::string, long long>& env_;
  size_t p_ = 0;
  void Ws() { while (p_ < e_.size() && e_[p_] == ' ') ++p_; }
  long long Sum() {
    long long v = Prod();
    for (;;) { Ws(); if (p_ < e_.size() && (e_[p_] == '+' || e_[p_] == '-')) { char o = e_[p_++]; long long r = Prod(); v = o == '+' ? v + r : v - r; } else return v; }
  }
  long long Prod() {
    long long v = Atom();
    for (;;) { Ws(); if (p_ < e_.size() && e_[p_] == '*') { ++p_; v *= Atom(); } else return v; }
  }
  long long Atom() {
    Ws();
    if (p_ >= e_.size()) throw std::runtime_error("bad expression '" + e_ + "'");
    if (e_[p_] == '-') { ++p_; return -Atom(); }
    if (e_[p_] == '(') { ++p_; long long v = Sum(); Ws(); if (p_ >= e_.size() || e_[p_] != ')') throw std::runtime_error("missing ')'"); ++p_; return v; }
    if (std::isdigit(static_cast<unsigned char>(e_[p_]))) { size_t n; long long v = std::stoll(e_.substr(p_), &n); p_ += n; return v; }
    size_t st = p_;
    while (p_ < e_.size() && (std::isalnum(static_cast<unsigned char>(e_[p_])) || e_[p_] == '_')) ++p_;
    auto it = env_.find(e_.substr(st, p_ - st));
    if (st == p_ || it == env_.end()) throw std::runtime_error("unknown name in '" + e_ + "'");
    return it->second;
  }
};

long long Ev(const std::string& e, const std::map<std::string, long long>& env) { return Eval(e, env).Run(); }

long long Gcd(long long a, long long b) { a = std::llabs(a); b = std::llabs(b); while (b) { long long t = a % b; a = b; b = t; } return a; }

}  // namespace

CertCheckResult CheckCertificate(const std::string& json_text, const std::string* external_source) {
  CertCheckResult r;
  auto pass = [&](const std::string& m) { r.passed.push_back(m); };
  auto fail = [&](const std::string& m) { r.ok = false; r.failures.push_back(m); };
  JV c;
  try { c = Reader(json_text).Parse(); } catch (const std::exception& e) { fail(std::string("certificate is not valid JSON: ") + e.what()); return r; }
  if (c["format"].s != "parallix-certificate" || c["version"].Int() != 1) { fail("not a parallix certificate (format/version)"); return r; }
  pass("format parallix-certificate v1");

  const std::string& text = c["source"]["text"].s;
  if (Sha256Hex(text) == c["source"]["sha256"].s) pass("embedded source text matches its sha256"); else fail("source hash mismatch: the embedded source text was altered");
  if (external_source) {
    if (Sha256Hex(*external_source) == c["source"]["sha256"].s) pass("on-disk source file matches the certificate hash");
    else fail("on-disk source file differs from the source this certificate was issued for");
  }

  const std::string verdict = c["verdict"].s, status = c["status"].s;
  bool unsafe = verdict == "UNSAFE";
  bool status_ok = unsafe ? (status == "REFUTED" || status == "UNSAFE_NOT_REFUTED")
                          : (status == "CORROBORATED" || status == "MISMATCH" || status == "NOT_VALIDATED");
  if (status_ok) pass("verdict " + verdict + " is consistent with status " + status); else fail("status " + status + " is impossible for verdict " + verdict);

  const JV& refs = c["references"];
  const JV& nest = c["loop_nest"];
  bool refs_in_source = true;
  for (const JV& ref : refs.a) if (text.find(ref["array"].s + "[") == std::string::npos) refs_in_source = false;
  if (refs_in_source) pass("every referenced array is subscripted in the source text"); else fail("a reference names an array that is never subscripted in the source");

  try {
    if (unsafe && status == "REFUTED") {
      const JV& ws = c["witnesses"];
      if (ws.a.empty()) fail("status REFUTED but no witness is recorded");
      for (size_t k = 0; k < ws.a.size(); ++k) {
        const JV& w = ws[k];
        std::string tag = "witness " + std::to_string(k + 1) + " (" + w["array"].s + ", " + w["type"].s + "): ";
        std::map<std::string, long long> dims;
        for (const auto& kv : w["dims"].o) dims[kv.first] = kv.second.Int();
        const JV* side[2] = {&w["first"], &w["second"]};
        bool good = true;
        long long outer[2] = {0, 0};
        bool is_write[2] = {false, false};
        for (int s = 0; s < 2; ++s) {
          const JV& ref = refs.a.at(static_cast<size_t>((*side[s])["ref"].Int()));
          is_write[s] = ref["kind"].s == "write";
          if (ref["array"].s != w["array"].s) good = false;
          std::map<std::string, long long> env = dims;
          for (const auto& kv : (*side[s])["iteration"].o) env[kv.first] = kv.second.Int();
          for (size_t l = 0; l < nest.a.size(); ++l) {
            const std::string var = nest[l]["var"].s;
            if (!env.count(var)) { good = false; continue; }
            long long lo = Ev(nest[l]["lower"].s, env), hi = Ev(nest[l]["upper"].s, env);
            if (env[var] < lo || env[var] >= hi) good = false;
            if (l == 0) outer[s] = env[var];
          }
          for (size_t d = 0; d < ref["subscripts"].a.size(); ++d) {
            if (Ev(ref["subscripts"][d].s, env) != w["element"][d].Int()) good = false;
          }
        }
        if (outer[0] == outer[1]) good = false;
        if (!is_write[0] && !is_write[1]) good = false;
        std::string want = is_write[0] && is_write[1] ? "WAW" : is_write[0] ? "RAW" : "WAR";
        if (want != w["type"].s) good = false;
        if (good) pass(tag + "both iterations are in bounds, differ in the outer loop, and their subscripts evaluate to the recorded element"); else fail(tag + "the named iterations do not demonstrate a conflict");
      }
    } else if (!unsafe) {
      size_t n = refs.a.size();
      const std::string outer = nest.a.empty() ? "" : nest[0]["var"].s;
      bool good = c["pairs"].a.size() == n * (n - 1) / 2;
      for (const JV& p : c["pairs"].a) {
        const JV& a = refs[static_cast<size_t>(p["a"].Int())];
        const JV& b = refs[static_cast<size_t>(p["b"].Int())];
        const std::string how = p["ruled_out_by"].s;
        if (p["dependent"].s == "true") {
          // Only a same-iteration pair may be dependent in a SAFE certificate: some dimension
          // has the same nonzero coefficient and offset on the outer index in both references,
          // so a shared element forces equal outer iterations.
          bool same = how == "same-iteration" && a["array"].s == b["array"].s;
          bool found = false;
          for (size_t d = 0; same && d < a["access"].a.size(); ++d) {
            const JV& x = a["access"][d];
            const JV& y = b["access"][d];
            if (x["coef"].Int() != 0 && x["coef"].Int() == y["coef"].Int() && x["var"].s == outer &&
                y["var"].s == outer && x["const"].Int() == y["const"].Int()) found = true;
          }
          good = good && same && found;
          continue;
        }
        if (how == "different-array") good = good && a["array"].s != b["array"].s;
        else if (how == "both-reads") good = good && a["kind"].s == "read" && b["kind"].s == "read";
        else if (how == "gcd") {
          bool excluded = false;
          for (size_t d = 0; d < a["access"].a.size(); ++d) {
            const JV& wa = a["kind"].s == "write" ? a : b;
            const JV& wb = a["kind"].s == "write" ? b : a;
            long long g = Gcd(wa["access"][d]["coef"].Int(), wb["access"][d]["coef"].Int());
            long long diff = wb["access"][d]["const"].Int() - wa["access"][d]["const"].Int();
            if (g == 0 ? diff != 0 : diff % g != 0) excluded = true;
          }
          good = good && excluded;
        } else good = false;
      }
      for (const JV& ref : refs.a) {
        if (ref["kind"].s != "write") continue;
        bool uses_outer = false;
        for (const JV& acc : ref["access"].a) if (acc["var"].s == outer && acc["coef"].Int() != 0) uses_outer = true;
        if (!uses_outer) good = false;
      }
      if (good) pass("absence of dependence re-derived: every pair is independent (different array, both reads, GCD, or confined to one iteration) and every write depends on the parallel index"); else fail("the recorded pairs do not support the claim that the loop is dependence-free");

      const JV& v = c["validation"];
      if (v.t == 'o') {
        long long ord = 0, par = 0, ord_bad = 0, par_bad = 0;
        for (const JV& cell : v["cells"].a) {
          bool is_par = cell["variant"].s == "par";
          (is_par ? par : ord) += cell["runs"].Int();
          (is_par ? par_bad : ord_bad) += cell["mismatches"].Int();
        }
        if (ord == v["order_runs"].Int() && par == v["parallel_runs"].Int() && ord_bad == v["order_mismatches"].Int() && par_bad == v["parallel_mismatches"].Int()) pass("run counts and mismatch counts equal the sums over the recorded cells"); else fail("validation totals do not equal the sum of the recorded runs");
        bool clean = ord_bad + par_bad == 0;
        if (clean == (status == "CORROBORATED")) pass("status agrees with the recorded mismatches"); else fail("status " + status + " contradicts the recorded mismatch counts");
      } else if (status != "NOT_VALIDATED") fail("status claims validation but the certificate records none");
    }
  } catch (const std::exception& e) {
    fail(std::string("malformed certificate content: ") + e.what());
  }
  r.notes.push_back("recorded test runs are reported, not re-executed; this check re-derives only the hash, the witness arithmetic and the independence claims");
  return r;
}

}  // namespace parallix
