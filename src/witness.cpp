#include "parallix/witness.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>

#include "parallix/render.h"

namespace parallix {

namespace {

using Env = std::map<std::string, long long>;

long long Eval(const AffineExpr& e, const Env& env) {
  long long v = e.constant;
  for (const auto& [name, coeff] : e.terms) {
    auto it = env.find(name);
    if (it != env.end()) v += coeff * it->second;
  }
  return v;
}

// All iteration vectors of the nest, in sequential order, capped at `cap`.
bool Enumerate(const PIRKernel& k, size_t level, Env& env, std::vector<long long>& cur,
               std::vector<std::vector<long long>>& out, size_t cap) {
  if (level == k.loop_nest.size()) {
    if (out.size() >= cap) return false;
    out.push_back(cur);
    return true;
  }
  const LoopLevel& lv = k.loop_nest[level];
  long long lo = Eval(lv.lower, env), hi = Eval(lv.upper, env);
  for (long long v = lo; v < hi; ++v) {
    env[lv.index_var] = v;
    cur.push_back(v);
    bool ok = Enumerate(k, level + 1, env, cur, out, cap);
    cur.pop_back();
    if (!ok) return false;
  }
  env.erase(lv.index_var);
  return true;
}

std::vector<long long> Element(const Reference& ref, const PIRKernel& k,
                               const std::vector<long long>& iter) {
  std::vector<long long> el;
  for (const AccessFunction& acc : ref.access) {
    long long v = acc.constant;
    if (acc.coefficient != 0) {
      for (size_t l = 0; l < k.loop_nest.size(); ++l) {
        if (k.loop_nest[l].index_var == acc.index_var) v += acc.coefficient * iter[l];
      }
    }
    el.push_back(v);
  }
  return el;
}

std::string IterText(const PIRKernel& k, const std::vector<std::pair<std::string, long long>>& it) {
  (void)k;
  std::ostringstream o;
  for (size_t i = 0; i < it.size(); ++i) o << (i ? ", " : "") << it[i].first << "=" << it[i].second;
  return o.str();
}

std::string ElementText(const std::string& array, const std::vector<long long>& el) {
  std::ostringstream o;
  o << array << "[";
  for (size_t i = 0; i < el.size(); ++i) o << (i ? ", " : "") << el[i];
  o << "]";
  return o.str();
}

}  // namespace

std::map<std::string, long long> DimsForSize(const std::vector<std::string>& dim_params,
                                             long long size) {
  std::map<std::string, long long> dims;
  for (size_t p = 0; p < dim_params.size(); ++p) {
    dims[dim_params[p]] = p == 0 ? size : size / (1LL << p) + 4;
  }
  return dims;
}

std::vector<Witness> FindWitnesses(const PIRKernel& kernel, const DependenceReport& report,
                                   const std::vector<std::string>& dim_params, long long size) {
  std::vector<Witness> result;
  const size_t kCap = 200000;
  // Shrink the problem until the iteration space is enumerable.
  std::vector<std::vector<long long>> iters;
  Env dims;
  for (;;) {
    dims = DimsForSize(dim_params, size);
    Env env = dims;
    std::vector<long long> cur;
    iters.clear();
    if (Enumerate(kernel, 0, env, cur, iters, kCap) || size <= 4) break;
    size /= 2;
  }

  for (size_t pi = 0; pi < report.pairs.size(); ++pi) {
    const PairResult& pr = report.pairs[pi];
    if (!pr.dependent) continue;
    Witness w;
    w.pair_index = pi;
    w.dims = dims;
    const Reference& ra = kernel.references[pr.ref_a_index];
    const Reference& rb = kernel.references[pr.ref_b_index];

    std::unordered_map<std::string, std::vector<size_t>> by_element;  // rb's element -> iteration ids
    auto key = [](const std::vector<long long>& el) {
      std::string s;
      for (long long v : el) s += std::to_string(v) + ",";
      return s;
    };
    for (size_t i = 0; i < iters.size(); ++i) {
      by_element[key(Element(rb, kernel, iters[i]))].push_back(i);
    }
    bool have = false;
    long long best_gap = 0, best_lo = 0;
    size_t bi = 0, bj = 0;
    for (size_t i = 0; i < iters.size(); ++i) {
      auto it = by_element.find(key(Element(ra, kernel, iters[i])));
      if (it == by_element.end()) continue;
      for (size_t j : it->second) {
        if (iters[i][0] == iters[j][0]) continue;  // same outer iteration: not loop-carried
        long long gap = std::llabs(iters[i][0] - iters[j][0]);
        long long lo = std::min(iters[i][0], iters[j][0]);
        if (!have || gap < best_gap || (gap == best_gap && lo < best_lo)) {
          have = true; best_gap = gap; best_lo = lo; bi = i; bj = j;
        }
      }
    }
    if (!have) {
      w.note = "no two iterations of '" + kernel.loop_nest[0].index_var +
               "' touch the same element at these dimensions; the verdict rests on the "
               "conservative analysis alone";
      result.push_back(std::move(w));
      continue;
    }
    // "first" = the iteration that runs earlier in sequential order.
    bool a_first = iters[bi][0] < iters[bj][0];
    size_t first_it = a_first ? bi : bj, second_it = a_first ? bj : bi;
    w.first_ref = a_first ? pr.ref_a_index : pr.ref_b_index;
    w.second_ref = a_first ? pr.ref_b_index : pr.ref_a_index;
    const Reference& f = kernel.references[w.first_ref];
    const Reference& s = kernel.references[w.second_ref];
    for (size_t l = 0; l < kernel.loop_nest.size(); ++l) {
      w.first_iter.push_back({kernel.loop_nest[l].index_var, iters[first_it][l]});
      w.second_iter.push_back({kernel.loop_nest[l].index_var, iters[second_it][l]});
    }
    w.array = f.array_name;
    w.element = Element(f, kernel, iters[first_it]);
    if (f.kind == RefKind::WRITE && s.kind == RefKind::WRITE) w.type = DependenceType::WAW;
    else if (f.kind == RefKind::WRITE) w.type = DependenceType::RAW;
    else w.type = DependenceType::WAR;
    w.found = true;

    std::ostringstream ex;
    ex << "loop-carried " << ToString(w.type) << " on '" << w.array << "': iteration ("
       << IterText(kernel, w.first_iter) << ") " << (f.kind == RefKind::WRITE ? "writes " : "reads ")
       << ElementText(w.array, w.element) << " via " << RenderReference(f) << "; iteration ("
       << IterText(kernel, w.second_iter) << ") " << (s.kind == RefKind::WRITE ? "writes " : "reads ")
       << "the same element via " << RenderReference(s) << ". ";
    switch (w.type) {
      case DependenceType::RAW:
        ex << "Sequentially the later iteration reads the earlier one's value; any order that runs "
              "it first reads a stale value.";
        break;
      case DependenceType::WAR:
        ex << "Sequentially the earlier iteration reads before the later one overwrites; any order "
              "that runs the writer first changes what is read.";
        break;
      default:
        ex << "Both iterations write the element; the surviving value depends on which runs last.";
    }
    w.explanation = ex.str();
    result.push_back(std::move(w));
  }
  return result;
}

}  // namespace parallix
