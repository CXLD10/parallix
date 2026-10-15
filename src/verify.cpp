#include "parallix/verify.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "parallix/compiler_driver.h"
#include "parallix/harness.h"

namespace parallix {

long long PlxLine::GetInt(const std::string& key, long long fallback) const {
  auto it = kv.find(key);
  if (it == kv.end()) return fallback;
  try {
    return std::stoll(it->second);
  } catch (...) {
    return fallback;
  }
}

double PlxLine::GetDouble(const std::string& key, double fallback) const {
  auto it = kv.find(key);
  if (it == kv.end()) return fallback;
  try {
    return std::stod(it->second);
  } catch (...) {
    return fallback;
  }
}

std::vector<PlxLine> ParsePlxOutput(const std::string& output) {
  std::vector<PlxLine> lines;
  std::istringstream in(output);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("PLX ", 0) != 0) continue;
    std::istringstream words(line.substr(4));
    PlxLine pl;
    words >> pl.kind;
    std::string tok;
    while (words >> tok) {
      size_t eq = tok.find('=');
      if (eq == std::string::npos) continue;
      pl.kv[tok.substr(0, eq)] = tok.substr(eq + 1);
    }
    lines.push_back(std::move(pl));
  }
  return lines;
}

BuiltHarness BuildHarness(const Analysis& a, const CodegenResult& gen, const Schedule& schedule,
                          const std::string& dir, const std::string& tag,
                          const std::string& extra_flags) {
  BuiltHarness built;
  std::filesystem::create_directories(dir);
  built.c_path = dir + "/" + tag + ".c";
  built.bin_path = dir + "/" + tag;
  HarnessResult h = GenerateHarness(a.kernel, a.pir, schedule, gen);
  {
    std::ofstream out(built.c_path);
    out << h.c_source;
  }
  built.compile = CompileC(built.c_path, built.bin_path, extra_flags, /*want_openmp=*/true);
  return built;
}

namespace {

std::string Join(const std::vector<int>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
  return s;
}
std::string JoinU(const std::vector<unsigned>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
  return s;
}

std::string ScalarArgs(const std::vector<std::pair<std::string, double>>& scalars) {
  std::string s;
  for (const auto& [name, value] : scalars) {
    std::ostringstream v;
    v << value;
    s += " --scalar " + name + "=" + v.str();
  }
  return s;
}

// The race-free control program: if ThreadSanitizer complains about *this*,
// the toolchain (typically an uninstrumented libgomp) cannot be trusted to
// judge the kernel, and the TSan step is skipped rather than reported.
const char* kTsanControl = R"(#include <stdio.h>
#include <stdlib.h>
int main(void) {
  int n = 4096;
  int *a = malloc(n * sizeof(int)), *b = malloc(n * sizeof(int));
  for (int i = 0; i < n; i++) a[i] = i;
  #pragma omp parallel for
  for (int i = 0; i < n; i++) b[i] = a[i] * 2;
  long s = 0;
  for (int i = 0; i < n; i++) s += b[i];
  printf("%ld\n", s);
  return 0;
}
)";

void RunTsan(const Analysis& a, const ValidationOptions& opts, ValidationReport* r) {
  const std::string flags = "-fsanitize=thread -g -fno-omit-frame-pointer";
  std::string ctl_c = opts.work_dir + "/tsan_control.c";
  std::string ctl_bin = opts.work_dir + "/tsan_control";
  {
    std::ofstream out(ctl_c);
    out << kTsanControl;
  }
  CompileResult cc = CompileC(ctl_c, ctl_bin, flags, true);
  if (!cc.ok || !cc.openmp) {
    r->tsan_status = "skipped";
    std::string first = cc.output.substr(0, cc.output.find('\n'));
    r->tsan_detail = cc.ok ? "OpenMP unavailable, nothing to check"
                           : "toolchain cannot build with -fsanitize=thread (" + first + ")";
    return;
  }
  std::string out;
  int code = RunCommand("\"" + ctl_bin + "\"", &out);
  if (code != 0 || out.find("ThreadSanitizer") != std::string::npos) {
    r->tsan_status = "skipped";
    std::string first = out.substr(0, out.find('\n'));
    r->tsan_detail =
        "ThreadSanitizer cannot judge OpenMP programs on this toolchain: its race-free control "
        "program already fails (exit " + std::to_string(code) + ": " + first + ")";
    return;
  }
  BuiltHarness h = BuildHarness(a, a.gen, a.schedule, opts.work_dir, a.kernel.name + "_tsan", flags);
  if (!h.compile.ok) {
    r->tsan_status = "skipped";
    r->tsan_detail = "TSan build of the kernel harness failed: " + h.compile.output.substr(0, 200);
    return;
  }
  std::string run;
  code = RunCommand("\"" + h.bin_path + "\" validate --sizes 37,101 --seeds 1 --threads 2,4 "
                    "--reps 3 --perms 0" + ScalarArgs(opts.scalars), &run);
  if (run.find("ThreadSanitizer") != std::string::npos) {
    r->tsan_status = "races";
    size_t p = run.find("WARNING");
    r->tsan_detail = run.substr(p == std::string::npos ? 0 : p, 300);
  } else if (code == 0) {
    r->tsan_status = "clean";
    r->tsan_detail = "sizes 37,101; seed 1; threads 2,4; 3 reps each; no race reported";
  } else {
    r->tsan_status = "skipped";
    r->tsan_detail = "TSan run exited with code " + std::to_string(code);
  }
}

}  // namespace

ValidationReport RunValidation(const Analysis& a, const ValidationOptions& opts) {
  ValidationReport r;
  if (a.schedule.kind == ScheduleKind::SEQUENTIAL_ONLY) {
    r.error = "no parallel variant exists for an UNSAFE kernel; nothing to validate";
    return r;
  }
  BuiltHarness h = BuildHarness(a, a.gen, a.schedule, opts.work_dir, a.kernel.name + "_harness");
  r.compiler = h.compile.compiler;
  r.compiler_version = h.compile.version;
  r.openmp = h.compile.openmp;
  if (!h.compile.ok) {
    r.error = h.compile.error;
    return r;
  }
  r.built = true;

  std::string cmd = "\"" + h.bin_path + "\" validate --sizes " + Join(opts.sizes) + " --seeds " +
                    JoinU(opts.seeds) + " --reps " + std::to_string(opts.reps) + " --perms " +
                    std::to_string(opts.perms);
  if (!opts.threads.empty()) cmd += " --threads " + Join(opts.threads);
  cmd += ScalarArgs(opts.scalars);
  std::string out;
  int code = RunCommand(cmd, &out);
  std::vector<PlxLine> lines = ParsePlxOutput(out);
  bool got_total = false;
  for (const PlxLine& l : lines) {
    if (l.kind == "config") {
      r.max_threads = static_cast<int>(l.GetInt("max_threads"));
      r.has_par = l.GetInt("has_par") != 0;
      r.reps = static_cast<int>(l.GetInt("reps"));
      r.perms = static_cast<int>(l.GetInt("perms"));
      std::istringstream sz(l.Get("sizes")), sd(l.Get("seeds")), th(l.Get("threads"));
      std::string t;
      while (std::getline(sz, t, ',')) r.sizes.push_back(std::stoi(t));
      while (std::getline(sd, t, ',')) r.seeds.push_back(static_cast<unsigned>(std::stoul(t)));
      while (std::getline(th, t, ',')) r.threads.push_back(std::stoi(t));
    } else if (l.kind == "cell") {
      ValidationCell c;
      c.variant = l.Get("variant");
      c.size = static_cast<int>(l.GetInt("size"));
      c.seed = static_cast<unsigned>(l.GetInt("seed"));
      c.threads = static_cast<int>(l.GetInt("threads"));
      c.team = static_cast<int>(l.GetInt("team"));
      c.runs = static_cast<int>(l.GetInt("runs"));
      c.mismatches = static_cast<int>(l.GetInt("mismatches"));
      r.cells.push_back(c);
    } else if (l.kind == "total") {
      got_total = true;
      r.order_runs = l.GetInt("order_runs");
      r.order_mismatches = l.GetInt("order_mismatches");
      r.par_runs = l.GetInt("par_runs");
      r.par_mismatches = l.GetInt("par_mismatches");
    } else if (l.kind == "first_mismatch") {
      std::ostringstream d;
      d << "variant=" << l.Get("variant") << " size=" << l.Get("size") << " seed=" << l.Get("seed")
        << " threads=" << l.Get("threads", "-") << " index=" << l.Get("index")
        << " expected=" << l.Get("expected") << " got=" << l.Get("got");
      r.first_mismatch = d.str();
    }
  }
  if (!got_total) {
    r.built = false;
    r.error = "harness produced no summary (exit " + std::to_string(code) + "):\n" + out;
    return r;
  }
  if (opts.tsan) RunTsan(a, opts, &r);
  return r;
}

std::string FormatValidation(const Analysis& a, const ValidationReport& r) {
  std::ostringstream o;
  if (!r.built) {
    o << "VALIDATION: not obtained -- " << r.error << "\n";
    return o.str();
  }
  std::set<int> distinct_sizes(r.sizes.begin(), r.sizes.end());
  o << "VALIDATION: order independence -- reversed + " << r.perms << " seeded permutations of loop '"
    << a.pir.loop_nest.front().index_var << "' x " << r.sizes.size() << " sizes (" << Join(r.sizes)
    << ") x " << r.seeds.size() << " seeds (" << JoinU(r.seeds) << ") = " << r.order_runs
    << " runs, " << r.order_mismatches << " mismatches\n";
  if (r.has_par) {
    o << "VALIDATION: parallel -- threads " << Join(r.threads) << " (machine max " << r.max_threads
      << ") x " << r.reps << " repetitions x " << r.sizes.size() << " sizes x " << r.seeds.size()
      << " seeds = " << r.par_runs << " runs, " << r.par_mismatches << " mismatches"
      << (r.openmp ? "" : " [built WITHOUT OpenMP: parallel variant ran single-threaded]") << "\n";
  }
  if (r.tsan_status == "not-requested") {
    o << "VALIDATION: ThreadSanitizer -- not requested\n";
  } else if (r.tsan_status == "skipped") {
    o << "VALIDATION: ThreadSanitizer -- skipped: " << r.tsan_detail << "\n";
  } else if (r.tsan_status == "clean") {
    o << "VALIDATION: ThreadSanitizer -- clean (" << r.tsan_detail << ")\n";
  } else {
    o << "VALIDATION: ThreadSanitizer -- RACE REPORTED: " << r.tsan_detail << "\n";
  }
  if (!r.first_mismatch.empty()) o << "VALIDATION: first mismatch -- " << r.first_mismatch << "\n";
  long long total = r.order_runs + r.par_runs;
  if (r.Passed()) {
    o << "CERTIFICATE: " << a.kernel.name << " STATICALLY SAFE, EMPIRICALLY CORROBORATED\n"
      << "  verdict " << ToString(a.report.verdict) << "; " << total << " runs over "
      << distinct_sizes.size() << " sizes, " << r.seeds.size() << " seeds, "
      << r.threads.size() << " thread counts, " << (r.perms + 1) << " iteration orders, "
      << r.reps << " repetitions each. Evidence for these inputs, not a proof for all inputs."
      << (r.openmp ? "" : " WARNING: built without OpenMP, so no real multi-threaded execution was tested; only the reordering checks are meaningful.")
      << "\n";
  } else {
    o << "CERTIFICATE MISMATCH: " << a.kernel.name
      << " parallel/permuted runs disagreed with the sequential result ("
      << (r.order_mismatches + r.par_mismatches) << " of " << total << " runs)\n";
  }
  return o.str();
}

namespace {

// Renders a flat row-major output index as e.g. "dp[3]" or "B[2, 5]".
std::string Coordinates(const Analysis& a, long long flat, int size) {
  const ArrayParamInfo* out = nullptr;
  for (const ArrayParamInfo& arr : a.gen.arrays) {
    if (arr.is_output) out = &arr;
  }
  if (!out || flat < 0) return "the scalar result";
  std::map<std::string, long long> dims = DimsForSize(a.gen.dim_params, size);
  std::vector<long long> extent;
  for (const Dim& d : out->dims) extent.push_back(d.is_ident ? dims[d.ident] : d.int_value);
  std::vector<long long> coord(extent.size());
  for (size_t k = extent.size(); k-- > 0;) {
    coord[k] = flat % extent[k];
    flat /= extent[k];
  }
  std::ostringstream o;
  o << out->name << "[";
  for (size_t k = 0; k < coord.size(); ++k) o << (k ? ", " : "") << coord[k];
  o << "]";
  return o.str();
}

}  // namespace

RefutationReport RunRefutation(const Analysis& a, const RefutationOptions& opts) {
  RefutationReport r;
  r.size = opts.size;
  r.seed = opts.seed;
  r.witnesses = FindWitnesses(a.pir, a.report, a.gen.dim_params, opts.size);

  Schedule sched = a.schedule;
  CodegenResult gen = a.gen;
  if (opts.force_parallel && sched.kind == ScheduleKind::SEQUENTIAL_ONLY) {
    sched.kind = a.pir.is_reduction ? ScheduleKind::PARALLEL_REDUCTION : ScheduleKind::PARALLEL_OUTER;
    sched.parallel_loop_index = 0;
    sched.reason = "forced by --force-parallel despite the UNSAFE verdict";
    gen = GenerateWith(a, sched);
  }
  BuiltHarness h = BuildHarness(a, gen, sched, opts.work_dir, a.kernel.name + "_refute");
  if (!h.compile.ok) {
    r.error = h.compile.error;
    return r;
  }
  r.built = true;
  std::string cmd = "\"" + h.bin_path + "\" refute --size " + std::to_string(opts.size) +
                    " --seed " + std::to_string(opts.seed) + " --reps " +
                    std::to_string(opts.forced_reps) + ScalarArgs(opts.scalars);
  std::string out;
  RunCommand(cmd, &out);
  for (const PlxLine& l : ParsePlxOutput(out)) {
    if (l.kind == "reverse") {
      r.diverged = l.GetInt("diverged") != 0;
      if (r.diverged) {
        r.seed = static_cast<unsigned>(l.GetInt("seed"));
        r.index = l.GetInt("index");
        r.sequential_value = l.GetDouble("sequential");
        r.reversed_value = l.GetDouble("reversed");
      }
    } else if (l.kind == "forced_parallel") {
      r.forced = true;
      r.forced_threads = static_cast<int>(l.GetInt("threads"));
      r.forced_team = static_cast<int>(l.GetInt("team"));
      r.forced_runs = static_cast<int>(l.GetInt("runs"));
      r.forced_differing = static_cast<int>(l.GetInt("differing"));
    }
  }
  if (r.diverged) r.location = Coordinates(a, r.index, opts.size);
  return r;
}

}  // namespace parallix

namespace parallix {

std::string FormatRefutation(const Analysis& a, const RefutationReport& r) {
  std::ostringstream o;
  if (!r.built) {
    o << "COUNTEREXAMPLE: not obtained -- " << r.error << "\n";
    return o.str();
  }
  const Witness* w = r.witnesses.empty() ? nullptr : &r.witnesses.front();
  if (w && w->found) {
    o << "WITNESS: " << w->explanation << "\n";
  } else {
    o << "WITNESS: none found at size " << r.size << (w ? " (" + w->note + ")" : "") << "\n";
  }
  if (r.diverged) {
    o << "COUNTEREXAMPLE (deterministic): size " << r.size << ", seed " << r.seed
      << ": running loop '" << a.pir.loop_nest.front().index_var
      << "' in reversed order instead of ascending order changes " << r.location
      << " -- first diverging index " << r.index << ": sequential=" << r.sequential_value
      << ", reversed=" << r.reversed_value << "\n";
  } else {
    o << "COUNTEREXAMPLE (deterministic): none -- reversed order gave the same result for the 8 "
         "seeds tried, so the UNSAFE verdict stands but is not refuted by this input\n";
  }
  if (r.forced) {
    o << "FORCED PARALLEL (nondeterministic; the reversed-order result above is the proof): "
      << r.forced_differing << " of " << r.forced_runs << " runs at " << r.forced_threads
      << " threads (OpenMP provided " << r.forced_team << ") differed from sequential\n";
  }
  if (r.Refuted()) {
    o << "CERTIFICATE: REFUTED -- " << a.kernel.name << " is not safe to parallelize: "
      << ToString(w->type) << " dependence on '" << w->array
      << "' witnessed by two concrete iterations, and reordering them changes the output\n";
  } else {
    o << "CERTIFICATE: UNSAFE, NOT REFUTED -- " << a.kernel.name
      << " was rejected by the analysis, but no concrete counterexample was produced\n";
  }
  return o.str();
}

}  // namespace parallix
