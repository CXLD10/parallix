#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

#include "parallix/bench.h"
#include "parallix/certcheck.h"
#include "parallix/certificate.h"
#include "parallix/compiler_driver.h"
#include "parallix/explain.h"
#include "parallix/hardware.h"
#include "parallix/pipeline.h"
#include "parallix/render.h"
#include "parallix/verify.h"
#include "parallix/version.h"
#include "parallix/witness.h"

// The parallix command-line driver. Subcommands:
//   analyze, explain, run, verify, bench, check-cert, demo
// `parallix file.plx` with no subcommand analyzes and then verifies the file.

namespace parallix {
namespace {

const char* kUsage =
    "parallix " "1.0.0" " -- an affine-loop compiler that certifies its own parallelization verdicts\n"
    "\n"
    "usage: parallix <command> [options]\n"
    "  analyze    <file.plx>              lex, parse, check, analyze dependences, choose a schedule\n"
    "  explain    <file.plx>              why each loop was (not) parallelized; writes an HTML report\n"
    "  run        <file.plx>              compile and execute the chosen schedule at --size\n"
    "  verify     <file.plx>              adversarial validation (safe kernels) or counterexample (unsafe)\n"
    "  bench      <file.plx>              median timings, sequential vs parallel; table, CSV, SVG chart\n"
    "  check-cert <cert.json>             independently re-check a certificate\n"
    "  demo                               guided two-part demonstration (see demo.sh)\n"
    "\n"
    "common options:\n"
    "  --out DIR            where generated C, certificates and reports go (default: generated)\n"
    "  --size N             problem size for the cost model / run (default: 10000)\n"
    "  --scalar NAME=VALUE  value for a scalar kernel parameter (repeatable)\n"
    "verify options:   --sizes a,b,c  --seeds a,b,c  --threads a,b,c  --reps R  --perms P  --no-tsan  --force-parallel\n"
    "bench options:    --sizes a,b,c  --threads a,b,c  --reps R (>=5)\n"
    "check-cert:       --source FILE  also compare against the on-disk source\n";

struct Args {
  std::string command;
  std::vector<std::string> positional;
  std::map<std::string, std::string> opt;
  std::vector<std::pair<std::string, double>> scalars;
  bool Has(const std::string& k) const { return opt.count(k) != 0; }
  std::string Get(const std::string& k, const std::string& d = "") const {
    auto it = opt.find(k);
    return it == opt.end() ? d : it->second;
  }
};

bool IsFlag(const std::string& k) {
  static const std::set<std::string> flags = {"--no-tsan", "--force-parallel", "--fast", "--help", "--version"};
  return flags.count(k) != 0;
}

bool ParseArgs(int argc, char** argv, Args* a, std::string* err) {
  for (int i = 1; i < argc; ++i) {
    std::string t = argv[i];
    if (t.rfind("--", 0) == 0) {
      if (IsFlag(t)) {
        a->opt[t] = "1";
        continue;
      }
      if (i + 1 >= argc) {
        *err = "option " + t + " needs a value";
        return false;
      }
      std::string v = argv[++i];
      if (t == "--scalar") {
        size_t eq = v.find('=');
        if (eq == std::string::npos) {
          *err = "--scalar expects NAME=VALUE";
          return false;
        }
        a->scalars.push_back({v.substr(0, eq), std::atof(v.c_str() + eq + 1)});
      } else {
        a->opt[t] = v;
      }
    } else if (a->command.empty()) {
      a->command = t;
    } else {
      a->positional.push_back(t);
    }
  }
  return true;
}

template <typename T>
std::vector<T> ParseList(const std::string& s) {
  std::vector<T> out;
  std::istringstream in(s);
  std::string tok;
  while (std::getline(in, tok, ',')) {
    if (!tok.empty()) out.push_back(static_cast<T>(std::stoll(tok)));
  }
  return out;
}

std::string OutDir(const Args& a) {
  std::string d = a.Get("--out", "generated");
  std::filesystem::create_directories(d);
  return d;
}

bool WriteFile(const std::string& path, const std::string& text) {
  std::ofstream out(path);
  out << text;
  return static_cast<bool>(out);
}

// Parses `path`; prints the stage error and returns false on failure.
bool Load(const std::string& path, ParsedSource* parsed) {
  std::string error;
  if (!ParseSourceFile(path, parsed, &error)) {
    std::cerr << error << "\n";
    return false;
  }
  return true;
}

void PrintAnalysis(const ParsedSource& parsed, const Analysis& a, const HardwareInfo& hw, long long size) {
  std::cout << "TOKENS: " << RenderTokens(parsed.tokens) << "\n";
  std::cout << "AST: " << RenderKernel(a.kernel) << "\n";
  std::cout << "SEMANTIC INFO: kernel '" << a.kernel.name
            << "' passed affine-boundedness, shape, reduction-validation, symbol-resolution and "
               "assignment-target checks.\n";
  std::ostringstream loops;
  loops << a.pir.loop_nest.size() << " loop" << (a.pir.loop_nest.size() == 1 ? "" : "s");
  if (!a.pir.loop_nest.empty()) {
    loops << " -- ";
    for (size_t i = 0; i < a.pir.loop_nest.size(); ++i) {
      const LoopLevel& lv = a.pir.loop_nest[i];
      loops << (i ? "; " : "") << lv.index_var << " (" << ToString(lv.mode) << ") in [" << lv.lower.ToString()
            << ", " << lv.upper.ToString() << ")";
    }
  }
  std::cout << "LOOP ANALYSIS: " << loops.str() << "\n";
  std::ostringstream dep;
  if (a.report.pairs.empty()) {
    dep << "no array reference pairs to compare";
    if (a.pir.is_reduction) {
      dep << " (the only reduction-related dependency is on the scalar accumulator '" << a.pir.reduce_accumulator << "')";
    }
  } else {
    dep << "comparing " << a.report.pairs.size() << " reference pair(s): ";
    for (size_t i = 0; i < a.report.pairs.size(); ++i) {
      const PairResult& pr = a.report.pairs[i];
      dep << (i ? "; " : "") << RenderReference(a.pir.references[pr.ref_a_index]);
      if (pr.ref_a_index != pr.ref_b_index) dep << " vs " << RenderReference(a.pir.references[pr.ref_b_index]);
      dep << ": " << pr.reasoning;
    }
  }
  std::cout << "DEPENDENCE ANALYSIS: " << dep.str() << "\n";
  std::cout << "PARALLELIZATION: " << ToString(a.report.verdict) << ", " << a.report.verdict_reason << "\n";
  std::cout << "SCHEDULE: " << ToString(a.schedule.kind) << " -- " << a.schedule.reason << "\n";
  if (a.schedule.kind != ScheduleKind::SEQUENTIAL_ONLY) {
    Profitability p = EstimateProfitability(a.kernel, a.pir, a.schedule, hw, DimsForSize(a.gen.dim_params, size));
    std::cout << "COST MODEL: " << p.reason << "\n";
  }
}

// Calls `fn` for every kernel in the file; semantic errors are printed and counted.
template <typename Fn>
int ForEachKernel(const std::string& path, Fn fn) {
  ParsedSource parsed;
  if (!Load(path, &parsed)) return 1;
  int status = 0;
  for (KernelDecl& k : parsed.program.kernels) {
    Analysis a;
    std::string error;
    if (!AnalyzeKernelDecl(std::move(k), &a, &error)) {
      std::cout << "TOKENS: " << RenderTokens(parsed.tokens) << "\n";
      std::cout << "SEMANTIC ERROR: " << error << "\n";
      status = 2;
      continue;
    }
    status = std::max(status, fn(parsed, a));
  }
  return status;
}

int DoAnalyze(const Args& a) {
  if (a.positional.empty()) { std::cerr << "usage: parallix analyze <file.plx>\n"; return 1; }
  HardwareInfo hw = DetectHardware();
  long long size = std::stoll(a.Get("--size", "10000"));
  return ForEachKernel(a.positional[0], [&](const ParsedSource& p, const Analysis& an) {
    PrintAnalysis(p, an, hw, size);
    return 0;
  });
}

int DoExplain(const Args& a) {
  if (a.positional.empty()) { std::cerr << "usage: parallix explain <file.plx>\n"; return 1; }
  HardwareInfo hw = DetectHardware();
  long long size = std::stoll(a.Get("--size", "10000"));
  std::string dir = OutDir(a);
  return ForEachKernel(a.positional[0], [&](const ParsedSource& p, const Analysis& an) {
    Explanation e = Explain(an, hw, size);
    std::cout << ExplainText(e);
    std::string path = a.Get("--html", dir + "/" + an.kernel.name + ".explain.html");
    WriteFile(path, ExplainHtml(e, p.text));
    std::cout << "\nHTML REPORT: " << path << "\n";
    return 0;
  });
}

int DoRun(const Args& a) {
  if (a.positional.empty()) { std::cerr << "usage: parallix run <file.plx>\n"; return 1; }
  HardwareInfo hw = DetectHardware();
  long long size = std::stoll(a.Get("--size", "10000"));
  std::string dir = OutDir(a);
  return ForEachKernel(a.positional[0], [&](const ParsedSource&, const Analysis& an) {
    Profitability p;
    bool par = false;
    int threads = 1;
    if (an.schedule.kind != ScheduleKind::SEQUENTIAL_ONLY) {
      p = EstimateProfitability(an.kernel, an.pir, an.schedule, hw, DimsForSize(an.gen.dim_params, size));
      par = p.profitable;
      threads = p.threads;
    }
    if (a.Has("--threads")) { threads = std::stoi(a.Get("--threads")); par = threads > 1 && an.schedule.kind != ScheduleKind::SEQUENTIAL_ONLY; }
    std::cout << "VERDICT: " << ToString(an.report.verdict) << "\n";
    if (an.schedule.kind == ScheduleKind::SEQUENTIAL_ONLY) {
      std::cout << "RUN: sequential (loop is not safe to parallelize)\n";
    } else if (par) {
      std::cout << "RUN: parallel on " << threads << " threads -- " << p.reason << "\n";
    } else {
      std::cout << "RUN: sequential -- " << (a.Has("--threads") ? "1 thread requested" : p.reason) << "\n";
    }
    BuiltHarness h = BuildHarness(an, an.gen, an.schedule, dir, an.kernel.name + "_run");
    if (!h.compile.ok) { std::cout << "EXECUTION: FAILED -- " << h.compile.error << "\n"; return 3; }
    std::string cmd = "\"" + h.bin_path + "\" run --size " + std::to_string(size) + " --threads " + std::to_string(threads);
    if (!par) cmd += " --no-par";
    for (const auto& [n, v] : a.scalars) { std::ostringstream o; o << v; cmd += " --scalar " + n + "=" + o.str(); }
    std::string out;
    int code = RunCommand(cmd, &out);
    for (const PlxLine& l : ParsePlxOutput(out)) {
      if (l.kind == "run") {
        std::cout << "EXECUTION: " << l.Get("variant") << " variant, " << l.Get("team") << " thread(s), size " << l.Get("size")
                  << ", " << l.Get("seconds") << " s\n";
      }
    }
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) if (line.rfind("PLX result ", 0) == 0) std::cout << "RESULT: " << line.substr(11) << "\n";
    return code == 0 ? 0 : 3;
  });
}

ValidationOptions MakeValidationOptions(const Args& a, const std::string& dir) {
  ValidationOptions v;
  v.work_dir = dir;
  if (a.Has("--sizes")) v.sizes = ParseList<int>(a.Get("--sizes"));
  if (a.Has("--seeds")) v.seeds = ParseList<unsigned>(a.Get("--seeds"));
  if (a.Has("--threads")) v.threads = ParseList<int>(a.Get("--threads"));
  if (a.Has("--reps")) v.reps = std::stoi(a.Get("--reps"));
  if (a.Has("--perms")) v.perms = std::stoi(a.Get("--perms"));
  v.tsan = !a.Has("--no-tsan");
  v.scalars = a.scalars;
  return v;
}

int DoVerify(const Args& a, bool with_analysis) {
  if (a.positional.empty()) { std::cerr << "usage: parallix verify <file.plx>\n"; return 1; }
  HardwareInfo hw = DetectHardware();
  std::string dir = OutDir(a);
  long long size = std::stoll(a.Get("--size", "10000"));
  return ForEachKernel(a.positional[0], [&](const ParsedSource& p, const Analysis& an) {
    if (with_analysis) PrintAnalysis(p, an, hw, size);
    else std::cout << "VERDICT: " << ToString(an.report.verdict) << " -- " << an.report.verdict_reason << "\n";
    CertificateInput in;
    in.source_path = a.positional[0];
    in.source_text = p.text;
    in.analysis = &an;
    in.hardware = hw;
    ValidationReport vr;
    RefutationReport rr;
    int status = 0;
    std::string text;
    if (an.report.verdict == Verdict::UNSAFE) {
      RefutationOptions ro;
      ro.work_dir = dir;
      ro.force_parallel = a.Has("--force-parallel");
      ro.scalars = a.scalars;
      if (a.Has("--reps")) ro.forced_reps = std::stoi(a.Get("--reps"));
      std::cout << "SCHEDULE: " << ToString(an.schedule.kind) << " -- no parallel code is generated for this kernel\n";
      rr = RunRefutation(an, ro);
      text = FormatRefutation(an, rr);
      in.refutation = &rr;
      if (!rr.Refuted()) status = 4;
    } else {
      vr = RunValidation(an, MakeValidationOptions(a, dir));
      std::cout << "GENERATED CODE: " << dir << "/" << an.kernel.name << "_harness.c\n";
      if (vr.built) {
        std::cout << "EXECUTION: compiled with '" << vr.compiler << "' (" << vr.compiler_version << "), OpenMP "
                  << (vr.openmp ? "enabled" : "NOT available") << "\n";
      }
      text = FormatValidation(an, vr);
      in.validation = &vr;
      if (!vr.Passed()) status = 4;
    }
    std::string cert_path = a.Get("--cert", dir + "/" + an.kernel.name + ".cert.json");
    WriteFile(cert_path, BuildCertificateJson(in));
    std::cout << "CERTIFICATE FILE: " << cert_path << "\n";
    std::cout << text;  // ends with the CERTIFICATE line
    return status;
  });
}

int DoBench(const Args& a) {
  if (a.positional.empty()) { std::cerr << "usage: parallix bench <file.plx> --sizes ... --threads ...\n"; return 1; }
  HardwareInfo hw = DetectHardware();
  std::string dir = OutDir(a);
  return ForEachKernel(a.positional[0], [&](const ParsedSource&, const Analysis& an) {
    BenchOptions bo;
    bo.work_dir = dir;
    if (a.Has("--sizes")) bo.sizes = ParseList<long long>(a.Get("--sizes"));
    if (a.Has("--threads")) bo.threads = ParseList<int>(a.Get("--threads"));
    if (a.Has("--reps")) bo.reps = std::stoi(a.Get("--reps"));
    bo.scalars = a.scalars;
    BenchResult r = RunBench(an, bo, hw);
    if (!r.ok) { std::cout << "BENCH FAILED: " << r.error << "\n"; return 3; }
    std::cout << BenchTable(r);
    std::string base = dir + "/" + an.kernel.name + ".bench";
    WriteFile(base + ".csv", BenchCsv(r));
    WriteFile(base + ".svg", BenchSvg(r));
    std::cout << "CSV: " << base << ".csv\nCHART: " << base << ".svg\n";
    for (const BenchRow& row : r.rows) if (!row.check_ok) { std::cout << "WARNING: a parallel result differed from sequential\n"; return 4; }
    return 0;
  });
}

int DoCheckCert(const Args& a) {
  if (a.positional.empty()) { std::cerr << "usage: parallix check-cert <cert.json> [--source file.plx]\n"; return 1; }
  std::ifstream in(a.positional[0]);
  if (!in) { std::cerr << "error: could not open '" << a.positional[0] << "'\n"; return 1; }
  std::ostringstream buf;
  buf << in.rdbuf();
  std::string src_text;
  const std::string* src = nullptr;
  if (a.Has("--source")) {
    std::ifstream s(a.Get("--source"));
    if (!s) { std::cerr << "error: could not open '" << a.Get("--source") << "'\n"; return 1; }
    std::ostringstream sb;
    sb << s.rdbuf();
    src_text = sb.str();
    src = &src_text;
  }
  CertCheckResult r = CheckCertificate(buf.str(), src);
  for (const std::string& p : r.passed) std::cout << "  ok    " << p << "\n";
  for (const std::string& f : r.failures) std::cout << "  FAIL  " << f << "\n";
  for (const std::string& n : r.notes) std::cout << "  note  " << n << "\n";
  std::cout << (r.ok ? "CERTIFICATE VALID" : "CERTIFICATE INVALID") << "\n";
  return r.ok ? 0 : 5;
}

int DoDemo(const Args& a, char** argv) {
  (void)argv;
  std::vector<std::string> candidates;
  if (const char* home = std::getenv("PARALLIX_HOME")) candidates.push_back(std::string(home) + "/demo.sh");
  candidates.push_back("demo.sh");
  candidates.push_back("../demo.sh");
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n > 0) {
    exe[n] = 0;
    std::filesystem::path bin = exe;
    candidates.push_back((bin.parent_path() / "../share/parallix/demo.sh").string());
    candidates.push_back((bin.parent_path().parent_path() / "demo.sh").string());
    candidates.push_back((bin.parent_path().parent_path().parent_path() / "demo.sh").string());
  }
  for (const std::string& c : candidates) {
    if (std::filesystem::exists(c)) {
      std::string cmd = "bash \"" + c + "\"" + (a.Has("--fast") ? " --fast" : "");
      return std::system(cmd.c_str()) == 0 ? 0 : 1;
    }
  }
  std::cerr << "demo.sh not found; run it from the repository root, or set PARALLIX_HOME\n";
  return 1;
}

}  // namespace
}  // namespace parallix

int main(int argc, char** argv) {
  using namespace parallix;
  Args a;
  std::string err;
  if (!ParseArgs(argc, argv, &a, &err)) { std::cerr << "error: " << err << "\n" << kUsage; return 1; }
  if (a.Has("--version") || a.command == "version") { std::cout << "parallix " << version() << "\n"; return 0; }
  if (a.command.empty() || a.Has("--help") || a.command == "help") { std::cout << kUsage; return a.command.empty() && !a.Has("--help") ? 1 : 0; }
  if (a.Has("--version") || a.command == "version") { std::cout << "parallix " << version() << "\n"; return 0; }
  try {
    if (a.command == "analyze") return DoAnalyze(a);
    if (a.command == "explain") return DoExplain(a);
    if (a.command == "run") return DoRun(a);
    if (a.command == "verify") return DoVerify(a, false);
    if (a.command == "bench") return DoBench(a);
    if (a.command == "check-cert") return DoCheckCert(a);
    if (a.command == "demo") return DoDemo(a, argv);
    // Legacy form: `parallix file.plx` analyzes and verifies.
    if (a.command.size() > 4 && a.command.substr(a.command.size() - 4) == ".plx") {
      a.positional.insert(a.positional.begin(), a.command);
      return DoVerify(a, true);
    }
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  std::cerr << "unknown command '" << a.command << "'\n" << kUsage;
  return 1;
}
