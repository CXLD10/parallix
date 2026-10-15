#include "parallix/bench.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>

#include "parallix/compiler_driver.h"
#include "parallix/schedule.h"
#include "parallix/witness.h"

namespace parallix {

BenchResult RunBench(const Analysis& a, const BenchOptions& opts, const HardwareInfo& hw) {
  BenchResult r;
  r.kernel = a.kernel.name;
  r.hardware = hw;
  if (a.schedule.kind == ScheduleKind::SEQUENTIAL_ONLY) {
    r.error = "kernel is UNSAFE: there is no parallel variant to benchmark";
    return r;
  }
  BuiltHarness h = BuildHarness(a, a.gen, a.schedule, opts.work_dir, a.kernel.name + "_bench");
  if (!h.compile.ok) {
    r.error = h.compile.error;
    return r;
  }
  std::vector<int> threads = opts.threads;
  if (threads.empty()) {
    std::set<int> s = {1, 2, 4, 8, hw.logical_cpus};
    threads.assign(s.begin(), s.end());
  }
  std::string tl;
  for (size_t i = 0; i < threads.size(); ++i) tl += (i ? "," : "") + std::to_string(threads[i]);
  int reps = std::max(opts.reps, 5);

  for (long long size : opts.sizes) {
    std::string cmd = "\"" + h.bin_path + "\" bench --size " + std::to_string(size) + " --seed " +
                      std::to_string(opts.seed) + " --reps " + std::to_string(reps) + " --threads " + tl;
    for (const auto& [n, v] : opts.scalars) {
      std::ostringstream o;
      o << v;
      cmd += " --scalar " + n + "=" + o.str();
    }
    std::string out;
    RunCommand(cmd, &out);
    double seq_median = 0;
    size_t first = r.rows.size();
    for (const PlxLine& l : ParsePlxOutput(out)) {
      if (l.kind != "bench") continue;
      BenchRow row;
      row.size = size;
      row.variant = l.Get("variant");
      row.threads = static_cast<int>(l.GetInt("threads"));
      row.team = static_cast<int>(l.GetInt("team"));
      row.reps = static_cast<int>(l.GetInt("reps"));
      row.median_s = l.GetDouble("median");
      row.min_s = l.GetDouble("min");
      row.max_s = l.GetDouble("max");
      row.check_ok = l.Get("check") == "ok";
      if (row.variant == "seq") seq_median = row.median_s;
      r.rows.push_back(row);
    }
    if (r.rows.size() == first || seq_median <= 0) {
      r.error = "benchmark harness produced no timings for size " + std::to_string(size) + ":\n" + out;
      return r;
    }
    for (size_t i = first; i < r.rows.size(); ++i) {
      r.rows[i].speedup = r.rows[i].median_s > 0 ? seq_median / r.rows[i].median_s : 0;
    }
    BenchSizeInfo info;
    info.size = size;
    Profitability p = EstimateProfitability(a.kernel, a.pir, a.schedule, hw,
                                            DimsForSize(a.gen.dim_params, size));
    info.model_seq_us = p.est_seq_us;
    info.model_threads = p.threads;
    info.model_profitable = p.profitable;
    info.model_reason = p.reason;
    r.sizes.push_back(info);
  }
  r.ok = true;
  return r;
}

std::string BenchTable(const BenchResult& r) {
  std::ostringstream o;
  o << "kernel " << r.kernel << " on " << (r.hardware.model.empty() ? "unknown CPU" : r.hardware.model)
    << " (" << r.hardware.logical_cpus << " logical CPUs)\n";
  o << std::left << std::setw(12) << "size" << std::setw(8) << "variant" << std::setw(9) << "threads"
    << std::right << std::setw(14) << "median (s)" << std::setw(12) << "speedup" << std::setw(12)
    << "efficiency" << "  check\n";
  for (const BenchRow& row : r.rows) {
    o << std::left << std::setw(12) << row.size << std::setw(8) << row.variant << std::setw(9) << row.threads
      << std::right << std::setw(14) << std::fixed << std::setprecision(6) << row.median_s
      << std::setw(11) << std::setprecision(2) << row.speedup << "x" << std::setw(11)
      << std::setprecision(0) << (100.0 * row.speedup / row.threads) << "%" << "  "
      << (row.check_ok ? "ok" : "MISMATCH") << "\n";
  }
  for (const BenchSizeInfo& s : r.sizes) {
    double measured = 0;
    for (const BenchRow& row : r.rows) {
      if (row.size == s.size && row.variant == "seq") measured = row.median_s * 1e6;
    }
    o << "model @ size " << s.size << ": predicted sequential " << std::setprecision(0) << s.model_seq_us
      << " us, measured " << measured << " us; recommends " << s.model_threads << " threads ("
      << (s.model_profitable ? "profitable" : "declines") << ")\n";
  }
  return o.str();
}

std::string BenchCsv(const BenchResult& r) {
  std::ostringstream o;
  o << "kernel,size,variant,threads,team,reps,median_s,min_s,max_s,speedup,check\n";
  for (const BenchRow& row : r.rows) {
    o << r.kernel << "," << row.size << "," << row.variant << "," << row.threads << "," << row.team << ","
      << row.reps << "," << std::setprecision(9) << std::fixed << row.median_s << "," << row.min_s << ","
      << row.max_s << "," << std::setprecision(4) << row.speedup << "," << (row.check_ok ? "ok" : "MISMATCH")
      << "\n";
  }
  return o.str();
}

std::string BenchSvg(const BenchResult& r) {
  // Okabe-Ito subset; identity is also carried by marker shape, a legend and
  // direct end-of-line labels, never by color alone.
  const char* colors[] = {"#0072B2", "#D55E00", "#009E73", "#CC79A7"};
  const double W = 720, H = 440, L = 64, R = 130, T = 64, B = 64;
  std::set<long long> size_set;
  double max_threads = 1, max_speed = 1;
  for (const BenchRow& row : r.rows) {
    if (row.variant != "par") continue;
    size_set.insert(row.size);
    max_threads = std::max<double>(max_threads, row.threads);
    max_speed = std::max(max_speed, row.speedup);
  }
  double ymax = std::ceil(std::max(max_speed, 2.0) * 1.1);
  double xmax = max_threads;
  auto X = [&](double t) { return L + (W - L - R) * (t / xmax); };
  auto Y = [&](double s) { return H - B - (H - T - B) * (s / ymax); };
  std::ostringstream o;
  o << std::fixed << std::setprecision(1);
  o << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << W << " " << H
    << "\" font-family=\"system-ui, sans-serif\" font-size=\"12\">\n"
    << "<rect width=\"" << W << "\" height=\"" << H << "\" fill=\"#fcfcfb\"/>\n"
    << "<text x=\"" << L << "\" y=\"26\" font-size=\"15\" font-weight=\"600\" fill=\"#1a1a19\">Speedup vs threads: "
    << r.kernel << "</text>\n"
    << "<text x=\"" << L << "\" y=\"44\" fill=\"#55554f\">median of " << (r.rows.empty() ? 0 : r.rows[0].reps)
    << " runs; " << r.hardware.logical_cpus << " logical CPUs; dashed = ideal linear speedup</text>\n";
  for (int g = 0; g <= static_cast<int>(ymax); g += std::max(1, static_cast<int>(ymax) / 6)) {
    o << "<line x1=\"" << L << "\" x2=\"" << W - R << "\" y1=\"" << Y(g) << "\" y2=\"" << Y(g)
      << "\" stroke=\"#e4e4e0\" stroke-width=\"1\"/>\n<text x=\"" << L - 8 << "\" y=\"" << Y(g) + 4
      << "\" text-anchor=\"end\" fill=\"#55554f\">" << g << "x</text>\n";
  }
  std::set<int> tick_threads;
  for (const BenchRow& row : r.rows) if (row.variant == "par") tick_threads.insert(row.threads);
  for (int t : tick_threads) {
    o << "<text x=\"" << X(t) << "\" y=\"" << H - B + 18 << "\" text-anchor=\"middle\" fill=\"#55554f\">" << t << "</text>\n";
  }
  o << "<text x=\"" << (L + W - R) / 2 << "\" y=\"" << H - 14 << "\" text-anchor=\"middle\" fill=\"#1a1a19\">threads</text>\n"
    << "<line x1=\"" << X(1) << "\" y1=\"" << Y(1) << "\" x2=\"" << X(std::min(xmax, ymax)) << "\" y2=\"" << Y(std::min(xmax, ymax))
    << "\" stroke=\"#8a8a84\" stroke-width=\"1\" stroke-dasharray=\"4 4\"/>\n";
  int idx = 0;
  for (long long size : size_set) {
    const char* col = colors[idx % 4];
    std::ostringstream pts;
    double lx = 0, ly = 0;
    for (const BenchRow& row : r.rows) {
      if (row.size != size || row.variant != "par") continue;
      pts << X(row.threads) << "," << Y(row.speedup) << " ";
      lx = X(row.threads);
      ly = Y(row.speedup);
    }
    o << "<polyline fill=\"none\" stroke=\"" << col << "\" stroke-width=\"2\" points=\"" << pts.str() << "\"/>\n";
    for (const BenchRow& row : r.rows) {
      if (row.size != size || row.variant != "par") continue;
      double cx = X(row.threads), cy = Y(row.speedup);
      if (idx % 2 == 0) {
        o << "<circle cx=\"" << cx << "\" cy=\"" << cy << "\" r=\"4.5\" fill=\"" << col << "\" stroke=\"#fcfcfb\" stroke-width=\"2\"/>\n";
      } else {
        o << "<rect x=\"" << cx - 4.5 << "\" y=\"" << cy - 4.5 << "\" width=\"9\" height=\"9\" fill=\"" << col
          << "\" stroke=\"#fcfcfb\" stroke-width=\"2\"/>\n";
      }
    }
    o << "<text x=\"" << lx + 10 << "\" y=\"" << ly + 4 << "\" fill=\"#1a1a19\">n=" << size << "</text>\n";
    // legend
    o << "<line x1=\"" << L + 150 * idx << "\" x2=\"" << L + 150 * idx + 18 << "\" y1=\"" << 58 << "\" y2=\"58\" stroke=\"" << col
      << "\" stroke-width=\"2\"/><text x=\"" << L + 150 * idx + 24 << "\" y=\"62\" fill=\"#1a1a19\">n=" << size << "</text>\n";
    ++idx;
  }
  o << "<line x1=\"" << L << "\" x2=\"" << W - R << "\" y1=\"" << Y(0) << "\" y2=\"" << Y(0) << "\" stroke=\"#8a8a84\"/>\n</svg>\n";
  return o.str();
}

}  // namespace parallix
