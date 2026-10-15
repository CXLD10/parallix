#include "parallix/explain.h"

#include <sstream>

#include "parallix/json.h"
#include "parallix/render.h"
#include "parallix/schedule.h"
#include "parallix/witness.h"

namespace parallix {

namespace {

std::string Header(const LoopLevel& lv) {
  return std::string(ToString(lv.mode)) + " " + lv.index_var + " in [" + lv.lower.ToString() + ", " +
         lv.upper.ToString() + ")";
}

std::string Escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else o += c;
  }
  return o;
}

}  // namespace

Explanation Explain(const Analysis& a, const HardwareInfo& hw, long long size) {
  Explanation e;
  e.kernel = a.kernel.name;
  e.verdict = ToString(a.report.verdict);
  e.verdict_reason = a.report.verdict_reason;
  e.schedule_summary = std::string(ToString(a.schedule.kind)) + " -- " + a.schedule.reason;

  Profitability profit;
  std::map<std::string, long long> dims = DimsForSize(a.gen.dim_params, size);
  if (a.schedule.kind != ScheduleKind::SEQUENTIAL_ONLY) {
    profit = EstimateProfitability(a.kernel, a.pir, a.schedule, hw, dims);
  }

  std::vector<std::string> pair_evidence;
  for (const PairResult& pr : a.report.pairs) {
    std::string a_txt = RenderReference(a.pir.references[pr.ref_a_index]);
    std::string b_txt = RenderReference(a.pir.references[pr.ref_b_index]);
    pair_evidence.push_back(a_txt + (pr.ref_a_index == pr.ref_b_index ? " (against itself across iterations)" : " vs " + b_txt) +
                            ": " + pr.reasoning);
  }
  if (a.pir.references.empty() && a.pir.is_reduction) {
    pair_evidence.push_back("no array reference pairs; the only carried dependence is the scalar accumulator '" +
                            a.pir.reduce_accumulator + "'");
  }

  for (size_t l = 0; l < a.pir.loop_nest.size(); ++l) {
    const LoopLevel& lv = a.pir.loop_nest[l];
    LoopDecision d;
    d.loop = lv.index_var;
    d.header = Header(lv);
    bool declared_parallel = lv.mode == LoopMode::PARALLEL;
    if (l == 0) {
      switch (a.schedule.kind) {
        case ScheduleKind::SEQUENTIAL_ONLY: {
          d.outcome = "kept sequential";
          d.rule = "R-UNSAFE";
          d.rule_text = "A loop with a loop-carried dependence that is not a licensed reduction is never parallelized.";
          d.evidence = pair_evidence;
          std::vector<Witness> ws = FindWitnesses(a.pir, a.report, a.gen.dim_params, 12);
          for (const Witness& w : ws) {
            if (w.found) { d.evidence.push_back("witness: " + w.explanation); break; }
          }
          d.rejected.push_back({"parallelize anyway", "iterations read values written by other iterations; the output would depend on thread timing (use `parallix verify --force-parallel` to see it fail)"});
          d.rejected.push_back({"reduction clause", a.pir.is_reduction ? "the carried dependence is on an array, not only the scalar accumulator" : "there is no scalar accumulator combined with an associative operator"});
          d.rejected.push_back({"loop skewing / pipelining", "not implemented in this version"});
          if (declared_parallel) e.notes.push_back("the source marks loop '" + lv.index_var + "' as `parallel`, but the dependence analysis overrides that annotation");
          break;
        }
        case ScheduleKind::PARALLEL_OUTER:
        case ScheduleKind::PARALLEL_REDUCTION: {
          bool red = a.schedule.kind == ScheduleKind::PARALLEL_REDUCTION;
          d.rule = red ? "R-REDUCTION" : "R-SAFE-OUTER";
          d.rule_text = red ? "SAFE_AS_REDUCTION: the only carried dependence is an associative accumulation; parallelize the reduction loop with a reduction clause."
                            : "SAFE: no pair of references conflicts across iterations; parallelize the outermost loop.";
          d.evidence = pair_evidence;
          d.evidence.push_back("verdict: " + e.verdict + " -- " + e.verdict_reason);
          char buf[160];
          std::snprintf(buf, sizeof buf, "cost model at %s: %.0f body executions x %.1f cycles each",
                        profit.dims.empty() ? "N=?" : "the chosen size", profit.iterations, profit.body_cycles);
          d.evidence.push_back(buf);
          d.evidence.push_back(profit.reason);
          if (profit.profitable) {
            d.outcome = "parallelized (OpenMP, recommended " + std::to_string(profit.threads) + " threads)";
          } else {
            d.outcome = "safe, but declined by the cost model";
            d.rule = "R-COST";
            d.rule_text = "A loop proven safe is still run sequentially when its estimated work does not repay the parallel overhead.";
          }
          if (!a.schedule.omp_schedule.empty()) {
            d.evidence.push_back("schedule(" + a.schedule.omp_schedule + "): an inner bound depends on '" + lv.index_var +
                                 "', so iterations carry unequal work and static chunks would unbalance the threads");
          }
          if (red) {
            d.rejected.push_back({"atomic / critical update of '" + a.pir.reduce_accumulator + "'", "serializes every update; a reduction clause keeps a private partial per thread and combines once"});
          }
          if (a.pir.loop_nest.size() > 1) {
            d.rejected.push_back({"parallelize inner loop '" + a.pir.loop_nest[1].index_var + "' instead", "would open a parallel region once per outer iteration, paying fork/join each time"});
            d.rejected.push_back({"collapse / tile the nest", "not implemented in this version"});
          }
          if (!a.schedule.omp_schedule.empty()) {
            d.rejected.push_back({"static schedule", "iterations are unequal; the thread holding the first chunk would do about twice the average work"});
          }
          if (lv.mode == LoopMode::SEQ) e.notes.push_back("loop '" + lv.index_var + "' is marked `seq` in the source; the analysis proved it safe and parallelizes it");
          break;
        }
      }
    } else {
      d.outcome = "kept sequential (inside each thread's iterations)";
      d.rule = "R-INNER-SEQ";
      d.rule_text = "Only the outermost loop is parallelized; inner loops run sequentially within each iteration of it.";
      d.evidence.push_back("outer loop '" + a.pir.loop_nest[0].index_var + "' already supplies the parallelism");
      d.rejected.push_back({"parallelize '" + lv.index_var + "' as well", "nested parallel regions add overhead without extra useful parallelism"});
      if (lv.mode == LoopMode::VECTORIZE) {
        d.evidence.push_back("`vectorize` annotation noted; the C compiler's own auto-vectorizer handles the inner loop at -O2 (no SIMD pragma is emitted)");
      }
    }
    e.loops.push_back(std::move(d));
  }
  return e;
}

std::string ExplainText(const Explanation& e) {
  std::ostringstream o;
  o << "EXPLANATION for kernel '" << e.kernel << "'\n";
  o << "  verdict : " << e.verdict << " -- " << e.verdict_reason << "\n";
  o << "  schedule: " << e.schedule_summary << "\n";
  for (const std::string& n : e.notes) o << "  note    : " << n << "\n";
  for (const LoopDecision& d : e.loops) {
    o << "\nLOOP " << d.loop << "   [" << d.header << "]\n";
    o << "  outcome : " << d.outcome << "\n";
    o << "  rule    : " << d.rule << " -- " << d.rule_text << "\n";
    for (const std::string& ev : d.evidence) o << "  evidence: " << ev << "\n";
    for (const auto& [alt, why] : d.rejected) o << "  rejected: " << alt << " -- " << why << "\n";
  }
  return o.str();
}

std::string ExplainHtml(const Explanation& e, const std::string& source_text) {
  std::ostringstream o;
  o << "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
       "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
       "<title>Parallix explanation: " << Escape(e.kernel) << "</title>\n<style>\n"
       ":root{--bg:#fcfcfb;--ink:#1a1a19;--muted:#55554f;--line:#e4e4e0;--card:#fff;--ok:#00704f;--no:#b3261e;--accent:#0072B2}\n"
       "@media (prefers-color-scheme: dark){:root{--bg:#161615;--ink:#ecece8;--muted:#a3a39b;--line:#2d2d2a;--card:#1f1f1d;--ok:#4cc79a;--no:#ff8a80;--accent:#56b4e9}}\n"
       "body{background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,sans-serif;margin:0;padding:24px 16px}\n"
       "main{max-width:820px;margin:0 auto}h1{font-size:22px;margin:0 0 4px}h2{font-size:16px;margin:0}\n"
       ".sub{color:var(--muted);margin:0 0 20px}.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:14px 16px;margin:0 0 14px}\n"
       ".tag{display:inline-block;font:12px ui-monospace,monospace;padding:1px 8px;border-radius:99px;border:1px solid var(--line);color:var(--muted);margin-left:8px}\n"
       ".ok{color:var(--ok)}.no{color:var(--no)}dl{margin:8px 0 0}dt{font-weight:600;margin-top:8px}dd{margin:2px 0 0;color:var(--muted)}\n"
       "pre{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px;overflow:auto;font-size:13px}code{font-family:ui-monospace,monospace}\n"
       "</style></head><body><main>\n";
  bool unsafe = e.verdict == "UNSAFE";
  o << "<h1>Kernel <code>" << Escape(e.kernel) << "</code></h1>\n<p class=\"sub\">Verdict <strong class=\""
    << (unsafe ? "no" : "ok") << "\">" << Escape(e.verdict) << (unsafe ? " (not parallelizable)" : "") << "</strong> &mdash; "
    << Escape(e.verdict_reason) << "</p>\n<p class=\"sub\">Schedule: " << Escape(e.schedule_summary) << "</p>\n";
  for (const std::string& n : e.notes) o << "<p class=\"sub\">Note: " << Escape(n) << "</p>\n";
  for (const LoopDecision& d : e.loops) {
    o << "<section class=\"card\"><h2>Loop <code>" << Escape(d.loop) << "</code><span class=\"tag\">" << Escape(d.header)
      << "</span></h2>\n<p><strong>" << Escape(d.outcome) << "</strong></p>\n<dl><dt>Rule " << Escape(d.rule)
      << "</dt><dd>" << Escape(d.rule_text) << "</dd>\n";
    if (!d.evidence.empty()) {
      o << "<dt>Evidence</dt>";
      for (const std::string& ev : d.evidence) o << "<dd>" << Escape(ev) << "</dd>";
    }
    if (!d.rejected.empty()) {
      o << "<dt>Alternatives rejected</dt>";
      for (const auto& [alt, why] : d.rejected) o << "<dd><strong>" << Escape(alt) << "</strong> &mdash; " << Escape(why) << "</dd>";
    }
    o << "</dl></section>\n";
  }
  o << "<h2>Source</h2>\n<pre><code>" << Escape(source_text) << "</code></pre>\n</main></body></html>\n";
  return o.str();
}

}  // namespace parallix
