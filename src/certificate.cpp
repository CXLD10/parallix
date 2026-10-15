#include "parallix/certificate.h"

#include "parallix/json.h"
#include "parallix/render.h"
#include "parallix/sha256.h"
#include "parallix/version.h"

namespace parallix {

namespace {

Json RefJson(const Reference& ref) {
  Json j = Json::Object();
  j.Set("text", RenderReference(ref));
  j.Set("array", ref.array_name);
  j.Set("kind", ToString(ref.kind));
  Json subs = Json::Array(), acc = Json::Array();
  for (const AccessFunction& a : ref.access) {
    subs.Add(RenderAccessFunction(a));
    Json one = Json::Object();
    one.Set("var", a.index_var);
    one.Set("coef", a.coefficient);
    one.Set("const", a.constant);
    acc.Add(one);
  }
  j.Set("subscripts", subs);
  j.Set("access", acc);
  return j;
}

Json IterJson(const std::vector<std::pair<std::string, long long>>& it) {
  Json j = Json::Object();
  for (const auto& [name, value] : it) j.Set(name, value);
  return j;
}

Json WitnessJson(const Witness& w, const PIRKernel& pir) {
  Json j = Json::Object();
  j.Set("pair", w.pair_index);
  j.Set("type", ToString(w.type));
  j.Set("array", w.array);
  Json dims = Json::Object();
  for (const auto& [k, v] : w.dims) dims.Set(k, v);
  j.Set("dims", dims);
  Json el = Json::Array();
  for (long long v : w.element) el.Add(v);
  j.Set("element", el);
  Json first = Json::Object(), second = Json::Object();
  first.Set("ref", w.first_ref);
  first.Set("iteration", IterJson(w.first_iter));
  second.Set("ref", w.second_ref);
  second.Set("iteration", IterJson(w.second_iter));
  j.Set("first", first);
  j.Set("second", second);
  j.Set("explanation", w.explanation);
  (void)pir;
  return j;
}

std::string RuledOutBy(const PairResult& pr, const PIRKernel& pir) {
  if (pr.dependent) {
    // Agreement in one dimension on the parallel loop's own index confines the
    // shared element to a single iteration (see the dependence engine).
    if (!pir.loop_nest.empty() && pr.ref_a_index != pr.ref_b_index) {
      const std::string& outer = pir.loop_nest.front().index_var;
      const Reference& ra = pir.references[pr.ref_a_index];
      const Reference& rb = pir.references[pr.ref_b_index];
      for (size_t d = 0; ra.array_name == rb.array_name && d < ra.access.size(); ++d) {
        const AccessFunction& x = ra.access[d];
        const AccessFunction& y = rb.access[d];
        if (x.coefficient != 0 && x.coefficient == y.coefficient && x.index_var == outer &&
            y.index_var == outer && x.constant == y.constant) {
          return "same-iteration";
        }
      }
    }
    return "";
  }
  const Reference& a = pir.references[pr.ref_a_index];
  const Reference& b = pir.references[pr.ref_b_index];
  if (a.array_name != b.array_name) return "different-array";
  if (a.kind == RefKind::READ && b.kind == RefKind::READ) return "both-reads";
  return "gcd";
}

}  // namespace

std::string CertificateStatus(const CertificateInput& in) {
  const Analysis& a = *in.analysis;
  if (a.report.verdict == Verdict::UNSAFE) {
    return in.refutation && in.refutation->Refuted() ? "REFUTED" : "UNSAFE_NOT_REFUTED";
  }
  if (!in.validation) return "NOT_VALIDATED";
  return in.validation->Passed() ? "CORROBORATED" : "MISMATCH";
}

std::string BuildCertificateJson(const CertificateInput& in) {
  const Analysis& a = *in.analysis;
  Json root = Json::Object();
  root.Set("format", "parallix-certificate");
  root.Set("version", 1);
  Json tool = Json::Object();
  tool.Set("name", "parallix");
  tool.Set("version", version());
  root.Set("tool", tool);

  Json src = Json::Object();
  src.Set("path", in.source_path);
  src.Set("sha256", Sha256Hex(in.source_text));
  src.Set("text", in.source_text);
  root.Set("source", src);

  Json cc = Json::Object();
  if (in.validation) {
    cc.Set("compiler", in.validation->compiler);
    cc.Set("version", in.validation->compiler_version);
    cc.Set("openmp", in.validation->openmp);
  } else if (in.refutation) {
    cc.Set("note", "counterexample harness compiled with the system C compiler");
  }
  root.Set("toolchain", cc);

  Json hw = Json::Object();
  hw.Set("logical_cpus", in.hardware.logical_cpus);
  hw.Set("model", in.hardware.model);
  hw.Set("l1d_bytes", in.hardware.l1d_bytes);
  hw.Set("l2_bytes", in.hardware.l2_bytes);
  hw.Set("l3_bytes", in.hardware.l3_bytes);
  root.Set("machine", hw);

  root.Set("kernel", a.kernel.name);
  root.Set("verdict", ToString(a.report.verdict));
  root.Set("verdict_reason", a.report.verdict_reason);
  root.Set("status", CertificateStatus(in));

  Json nest = Json::Array();
  for (const LoopLevel& lv : a.pir.loop_nest) {
    Json l = Json::Object();
    l.Set("var", lv.index_var);
    l.Set("lower", lv.lower.ToString());
    l.Set("upper", lv.upper.ToString());
    l.Set("mode", ToString(lv.mode));
    nest.Add(l);
  }
  root.Set("loop_nest", nest);

  Json refs = Json::Array();
  for (const Reference& r : a.pir.references) refs.Add(RefJson(r));
  root.Set("references", refs);

  Json pairs = Json::Array();
  for (const PairResult& pr : a.report.pairs) {
    Json p = Json::Object();
    p.Set("a", pr.ref_a_index);
    p.Set("b", pr.ref_b_index);
    p.Set("dependent", pr.dependent);
    p.Set("type", ToString(pr.type));
    if (pr.distance_known) p.Set("distance", pr.distance);
    p.Set("ruled_out_by", RuledOutBy(pr, a.pir));
    p.Set("reasoning", pr.reasoning);
    pairs.Add(p);
  }
  root.Set("pairs", pairs);

  Json sched = Json::Object();
  sched.Set("kind", ToString(a.schedule.kind));
  sched.Set("loop", a.schedule.kind == ScheduleKind::SEQUENTIAL_ONLY
                        ? std::string()
                        : a.pir.loop_nest[a.schedule.parallel_loop_index].index_var);
  sched.Set("reason", a.schedule.reason);
  root.Set("schedule", sched);

  if (a.report.verdict == Verdict::UNSAFE) {
    Json ws = Json::Array();
    if (in.refutation) {
      for (const Witness& w : in.refutation->witnesses) {
        if (w.found) ws.Add(WitnessJson(w, a.pir));
      }
    }
    root.Set("witnesses", ws);
    if (in.refutation) {
      const RefutationReport& r = *in.refutation;
      Json ref = Json::Object();
      ref.Set("size", r.size);
      ref.Set("diverged", r.diverged);
      if (r.diverged) {
        ref.Set("seed", r.seed);
        ref.Set("first_diverging_index", r.index);
        ref.Set("location", r.location);
        ref.Set("sequential_value", r.sequential_value);
        ref.Set("reversed_value", r.reversed_value);
      }
      if (r.forced) {
        Json f = Json::Object();
        f.Set("nondeterministic", true);
        f.Set("threads", r.forced_threads);
        f.Set("runs", r.forced_runs);
        f.Set("differing", r.forced_differing);
        ref.Set("forced_parallel", f);
      }
      root.Set("refutation", ref);
    }
  } else {
    root.Set("absence_summary",
             std::to_string(a.report.pairs.size()) +
                 " reference pair(s) examined; none conflicts across iterations of the "
                 "parallelized loop");
    if (in.validation) {
      const ValidationReport& v = *in.validation;
      Json val = Json::Object();
      Json sizes = Json::Array(), seeds = Json::Array(), threads = Json::Array();
      for (int s : v.sizes) sizes.Add(s);
      for (unsigned s : v.seeds) seeds.Add(s);
      for (int t : v.threads) threads.Add(t);
      val.Set("sizes", sizes);
      val.Set("seeds", seeds);
      val.Set("threads", threads);
      val.Set("repetitions", v.reps);
      val.Set("permutations", v.perms);
      val.Set("machine_max_threads", v.max_threads);
      Json cells = Json::Array();
      for (const ValidationCell& c : v.cells) {
        Json one = Json::Object();
        one.Set("variant", c.variant);
        one.Set("size", c.size);
        one.Set("seed", c.seed);
        one.Set("threads", c.threads);
        if (c.team) one.Set("team", c.team);
        one.Set("runs", c.runs);
        one.Set("mismatches", c.mismatches);
        cells.Add(one);
      }
      val.Set("cells", cells);
      val.Set("order_runs", v.order_runs);
      val.Set("order_mismatches", v.order_mismatches);
      val.Set("parallel_runs", v.par_runs);
      val.Set("parallel_mismatches", v.par_mismatches);
      Json ts = Json::Object();
      ts.Set("status", v.tsan_status);
      ts.Set("detail", v.tsan_detail);
      val.Set("thread_sanitizer", ts);
      root.Set("validation", val);
    }
  }
  return root.Dump(2) + "\n";
}

}  // namespace parallix
