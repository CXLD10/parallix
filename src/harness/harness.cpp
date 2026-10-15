#include "parallix/harness.h"

#include <sstream>
#include <vector>

namespace parallix {

namespace {

// The generated program is a fixed, kernel-independent runtime (kRuntime)
// glued to a few kernel-specific definitions emitted by Glue(). Commands:
//
//   validate  order-independence + multi-thread differential testing
//   refute    reversed-order divergence and forced-parallel repetition
//   bench     median wall-clock timing, sequential vs parallel
//
// Results are printed as `PLX key=value ...` lines for the driver to parse.

const char* kPreamble = R"PLX(#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _OPENMP
#include <omp.h>
#else
static int omp_get_max_threads(void) { return 1; }
static void omp_set_num_threads(int n) { (void)n; }
#include <time.h>
static double omp_get_wtime(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}
#endif
#if defined(__GNUC__) || defined(__clang__)
#define PLX_UNUSED __attribute__((unused))
#else
#define PLX_UNUSED
#endif

/* Deterministic, seeded 32-bit mixing: the same (index, salt) always yields the same value. */
static PLX_UNUSED unsigned plx_mix(unsigned x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}
static PLX_UNUSED void plx_fill_float(float* b, size_t n, unsigned salt) {
  for (size_t i = 0; i < n; i++) b[i] = (float)(plx_mix((unsigned)i * 2654435761u + salt) % 1000) / 10.0f;
}
static PLX_UNUSED void plx_fill_double(double* b, size_t n, unsigned salt) {
  for (size_t i = 0; i < n; i++) b[i] = (double)(plx_mix((unsigned)i * 2654435761u + salt) % 1000) / 10.0;
}
static PLX_UNUSED void plx_fill_int(int* b, size_t n, unsigned salt) {
  for (size_t i = 0; i < n; i++) b[i] = (int)(plx_mix((unsigned)i * 2654435761u + salt) % 100);
}
static PLX_UNUSED void plx_fill_ll(long long* b, size_t n, unsigned salt) {
  for (size_t i = 0; i < n; i++) b[i] = (long long)(plx_mix((unsigned)i * 2654435761u + salt) % 100);
}

typedef struct { long long i; double d; } PlxRet;
)PLX";

const char* kRuntime = R"PLX(
#define PLX_MAXLIST 32
typedef struct {
  int sizes[PLX_MAXLIST]; int n_sizes;
  unsigned seeds[PLX_MAXLIST]; int n_seeds;
  int threads[PLX_MAXLIST]; int n_threads;
  int reps, perms, size; unsigned seed;
  int no_par;
  const char* ov_name[8]; double ov_val[8]; int n_ov;
} PlxOpts;
static PlxOpts plx_opts;

static int plx_parse_list(const char* s, long long* out, int cap) {
  int n = 0;
  while (*s && n < cap) {
    char* end;
    out[n++] = strtoll(s, &end, 10);
    if (end == s) { n--; break; }
    s = (*end == ',') ? end + 1 : end;
  }
  return n;
}

static PLX_UNUSED double plx_scalar(const char* name, unsigned seed, int idx, int is_float) {
  for (int k = 0; k < plx_opts.n_ov; k++)
    if (strcmp(plx_opts.ov_name[k], name) == 0) return plx_opts.ov_val[k];
  unsigned x = plx_mix(seed * 7919u + (unsigned)idx * 104729u + 17u);
  return is_float ? (double)(x % 1000) / 10.0 : (double)(x % 200);
}

static PLX_UNUSED void plx_make_order(int* order, int trip, unsigned seed, int kind) {
  for (int i = 0; i < trip; i++) order[i] = (kind == 0) ? trip - 1 - i : i;
  if (kind == 0) return;
  unsigned s = plx_mix(seed * 2654435761u + (unsigned)kind * 40503u + 1u);
  for (int i = trip - 1; i > 0; i--) {
    s = plx_mix(s + 0x9e3779b9u);
    int j = (int)(s % (unsigned)(i + 1));
    int t = order[i]; order[i] = order[j]; order[j] = t;
  }
}

static PLX_UNUSED int plx_team_size(int threads) {
  int seen = 1;
  omp_set_num_threads(threads);
#ifdef _OPENMP
  #pragma omp parallel
  {
    #pragma omp single
    seen = omp_get_num_threads();
  }
#endif
  return seen;
}

static void plx_default_threads(void) {
  int mx = omp_get_max_threads();
  int cand[5] = {1, 2, 4, 8, mx};
  plx_opts.n_threads = 0;
  for (int c = 0; c < 5; c++) {
    int dup = 0;
    for (int k = 0; k < plx_opts.n_threads; k++) if (plx_opts.threads[k] == cand[c]) dup = 1;
    if (!dup) plx_opts.threads[plx_opts.n_threads++] = cand[c];
  }
}

static int plx_cmp_double(const void* a, const void* b) {
  double x = *(const double*)a, y = *(const double*)b;
  return (x > y) - (x < y);
}

static PLX_UNUSED int plx_cmd_validate(void) {
  int max_threads = omp_get_max_threads();
  printf("PLX config sizes=");
  for (int k = 0; k < plx_opts.n_sizes; k++) printf(k ? ",%d" : "%d", plx_opts.sizes[k]);
  printf(" seeds=");
  for (int k = 0; k < plx_opts.n_seeds; k++) printf(k ? ",%u" : "%u", plx_opts.seeds[k]);
  printf(" threads=");
  for (int k = 0; k < plx_opts.n_threads; k++) printf(k ? ",%d" : "%d", plx_opts.threads[k]);
  printf(" reps=%d perms=%d max_threads=%d has_par=%d\n", plx_opts.reps, plx_opts.perms,
         max_threads, PLX_HAS_PAR && !plx_opts.no_par);

  long long order_runs = 0, order_bad = 0, par_runs = 0, par_bad = 0;
  int printed_first = 0;
  for (int si = 0; si < plx_opts.n_sizes; si++) {
    PlxDims d; plx_dims(&d, plx_opts.sizes[si]);
    int trip = plx_trip(&d);
    int* order = (int*)malloc(sizeof(int) * (size_t)(trip > 0 ? trip : 1));
    for (int ki = 0; ki < plx_opts.n_seeds; ki++) {
      unsigned seed = plx_opts.seeds[ki];
      PlxArgs master; plx_make(&master, &d, seed);
      PlxArgs ref; plx_clone(&ref, &master, &d);
      PlxRet rr = plx_call_seq(&ref, &d);
      for (int kind = 0; kind <= plx_opts.perms && trip > 0; kind++) {
        PlxArgs w; plx_clone(&w, &master, &d);
        plx_make_order(order, trip, seed, kind);
        PlxRet r = plx_call_ord(&w, &d, order);
        long long idx; double va, vb;
        int ok = plx_equal(&ref, rr, &w, r, &d, &idx, &va, &vb);
        order_runs++; if (!ok) order_bad++;
        if (kind == 0) printf("PLX cell variant=reverse"); else printf("PLX cell variant=perm%d", kind);
        printf(" size=%d seed=%u threads=0 runs=1 mismatches=%d\n", plx_opts.sizes[si], seed, !ok);
        if (!ok && !printed_first) {
          printed_first = 1;
          printf("PLX first_mismatch variant=order%d size=%d seed=%u index=%lld expected=%.17g got=%.17g\n",
                 kind, plx_opts.sizes[si], seed, idx, va, vb);
        }
        plx_free(&w);
      }
      if (PLX_HAS_PAR && !plx_opts.no_par) {
        for (int ti = 0; ti < plx_opts.n_threads; ti++) {
          int t = plx_opts.threads[ti];
          int team = plx_team_size(t);
          int bad = 0;
          for (int rep = 0; rep < plx_opts.reps; rep++) {
            PlxArgs w; plx_clone(&w, &master, &d);
            PlxRet r = plx_call_par(&w, &d);
            long long idx; double va, vb;
            int ok = plx_equal(&ref, rr, &w, r, &d, &idx, &va, &vb);
            par_runs++;
            if (!ok) {
              par_bad++; bad++;
              if (!printed_first) {
                printed_first = 1;
                printf("PLX first_mismatch variant=par size=%d seed=%u threads=%d index=%lld expected=%.17g got=%.17g\n",
                       plx_opts.sizes[si], seed, t, idx, va, vb);
              }
            }
            plx_free(&w);
          }
          printf("PLX cell variant=par size=%d seed=%u threads=%d team=%d runs=%d mismatches=%d\n",
                 plx_opts.sizes[si], seed, t, team, plx_opts.reps, bad);
        }
      }
      plx_free(&ref); plx_free(&master);
    }
    free(order);
  }
  printf("PLX total order_runs=%lld order_mismatches=%lld par_runs=%lld par_mismatches=%lld\n",
         order_runs, order_bad, par_runs, par_bad);
  return (order_bad + par_bad) == 0 ? 0 : 1;
}

static PLX_UNUSED int plx_cmd_refute(void) {
  PlxDims d; plx_dims(&d, plx_opts.size);
  int trip = plx_trip(&d);
  printf("PLX refute_config size=%d trip=%d\n", plx_opts.size, trip);
  int found = 0;
  unsigned used_seed = plx_opts.seed;
  if (trip > 0) {
    int* order = (int*)malloc(sizeof(int) * (size_t)trip);
    for (int attempt = 0; attempt < 8 && !found; attempt++) {
      unsigned seed = plx_opts.seed + (unsigned)attempt;
      PlxArgs master; plx_make(&master, &d, seed);
      PlxArgs ref; plx_clone(&ref, &master, &d);
      PlxRet rr = plx_call_seq(&ref, &d);
      PlxArgs w; plx_clone(&w, &master, &d);
      plx_make_order(order, trip, seed, 0);
      PlxRet r = plx_call_ord(&w, &d, order);
      long long idx; double va, vb;
      if (!plx_equal(&ref, rr, &w, r, &d, &idx, &va, &vb)) {
        found = 1; used_seed = seed;
        printf("PLX reverse diverged=1 seed=%u index=%lld sequential=%.17g reversed=%.17g\n",
               seed, idx, va, vb);
      }
      plx_free(&w); plx_free(&ref); plx_free(&master);
    }
    free(order);
  }
  if (!found) printf("PLX reverse diverged=0 seeds_tried=8\n");
  if (PLX_HAS_PAR && !plx_opts.no_par && trip > 0) {
    int t = plx_opts.threads[plx_opts.n_threads - 1];
    int team = plx_team_size(t);
    PlxArgs master; plx_make(&master, &d, used_seed);
    PlxArgs ref; plx_clone(&ref, &master, &d);
    PlxRet rr = plx_call_seq(&ref, &d);
    int bad = 0;
    for (int rep = 0; rep < plx_opts.reps; rep++) {
      PlxArgs w; plx_clone(&w, &master, &d);
      PlxRet r = plx_call_par(&w, &d);
      long long idx; double va, vb;
      if (!plx_equal(&ref, rr, &w, r, &d, &idx, &va, &vb)) bad++;
      plx_free(&w);
    }
    printf("PLX forced_parallel size=%d seed=%u threads=%d team=%d runs=%d differing=%d\n",
           plx_opts.size, used_seed, t, team, plx_opts.reps, bad);
    plx_free(&ref); plx_free(&master);
  }
  return 0;
}

static PLX_UNUSED int plx_cmd_bench(void) {
  PlxDims d; plx_dims(&d, plx_opts.size);
  PlxArgs master; plx_make(&master, &d, plx_opts.seed);
  PlxArgs ref; plx_clone(&ref, &master, &d);
  PlxRet rr = plx_call_seq(&ref, &d);
  int reps = plx_opts.reps;
  double* t = (double*)malloc(sizeof(double) * (size_t)reps);
  int nv = (PLX_HAS_PAR && !plx_opts.no_par) ? plx_opts.n_threads : 0;
  for (int v = -1; v < nv; v++) {
    int th = v < 0 ? 1 : plx_opts.threads[v];
    int team = v < 0 ? 1 : plx_team_size(th);
    int all_ok = 1;
    for (int rep = -1; rep < reps; rep++) {  /* rep -1 is an untimed warm-up */
      PlxArgs w; plx_clone(&w, &master, &d);
      double t0 = omp_get_wtime();
      PlxRet r = v < 0 ? plx_call_seq(&w, &d) : plx_call_par(&w, &d);
      double t1 = omp_get_wtime();
      long long idx; double va, vb;
      if (!plx_equal(&ref, rr, &w, r, &d, &idx, &va, &vb)) all_ok = 0;
      if (rep >= 0) t[rep] = t1 - t0;
      plx_free(&w);
    }
    qsort(t, (size_t)reps, sizeof(double), plx_cmp_double);
    double med = (reps % 2) ? t[reps / 2] : 0.5 * (t[reps / 2 - 1] + t[reps / 2]);
    printf("PLX bench variant=%s threads=%d team=%d reps=%d median=%.9f min=%.9f max=%.9f check=%s\n",
           v < 0 ? "seq" : "par", th, team, reps, med, t[0], t[reps - 1], all_ok ? "ok" : "MISMATCH");
  }
  free(t);
  plx_free(&ref); plx_free(&master);
  return 0;
}

static PLX_UNUSED int plx_cmd_run(void) {
  PlxDims d; plx_dims(&d, plx_opts.size);
  PlxArgs w; plx_make(&w, &d, plx_opts.seed);
  int par = PLX_HAS_PAR && !plx_opts.no_par;
  int th = par ? plx_opts.threads[plx_opts.n_threads - 1] : 1;
  int team = par ? plx_team_size(th) : 1;
  double t0 = omp_get_wtime();
  PlxRet r = par ? plx_call_par(&w, &d) : plx_call_seq(&w, &d);
  double t1 = omp_get_wtime();
  char buf[256];
  plx_summary(&w, r, &d, buf, sizeof buf);
  printf("PLX run variant=%s threads=%d team=%d size=%d seed=%u seconds=%.9f\n", par ? "par" : "seq", th, team,
         plx_opts.size, plx_opts.seed, t1 - t0);
  printf("PLX result %s\n", buf);
  plx_free(&w);
  return 0;
}

int main(int argc, char** argv) {
  long long tmp[PLX_MAXLIST];
  plx_opts.n_sizes = 3; plx_opts.sizes[0] = 37; plx_opts.sizes[1] = 101; plx_opts.sizes[2] = 257;
  plx_opts.n_seeds = 3; plx_opts.seeds[0] = 1; plx_opts.seeds[1] = 2; plx_opts.seeds[2] = 3;
  plx_opts.reps = 20; plx_opts.perms = 3; plx_opts.size = 12; plx_opts.seed = 1;
  plx_default_threads();
  const char* cmd = PLX_HAS_PAR ? "validate" : "refute";
  int first = 1;
  if (argc > 1 && argv[1][0] != '-') { cmd = argv[1]; first = 2; }
  for (int a = first; a < argc; a++) {
    const char* k = argv[a];
    const char* v = (a + 1 < argc) ? argv[a + 1] : "";
    if (!strcmp(k, "--sizes")) { int n = plx_parse_list(v, tmp, PLX_MAXLIST); for (int i = 0; i < n; i++) plx_opts.sizes[i] = (int)tmp[i]; plx_opts.n_sizes = n; a++; }
    else if (!strcmp(k, "--seeds")) { int n = plx_parse_list(v, tmp, PLX_MAXLIST); for (int i = 0; i < n; i++) plx_opts.seeds[i] = (unsigned)tmp[i]; plx_opts.n_seeds = n; a++; }
    else if (!strcmp(k, "--threads")) { int n = plx_parse_list(v, tmp, PLX_MAXLIST); for (int i = 0; i < n; i++) plx_opts.threads[i] = (int)tmp[i]; plx_opts.n_threads = n; a++; }
    else if (!strcmp(k, "--reps")) { plx_opts.reps = atoi(v); a++; }
    else if (!strcmp(k, "--perms")) { plx_opts.perms = atoi(v); a++; }
    else if (!strcmp(k, "--size")) { plx_opts.size = atoi(v); a++; }
    else if (!strcmp(k, "--seed")) { plx_opts.seed = (unsigned)atoi(v); a++; }
    else if (!strcmp(k, "--no-par")) { plx_opts.no_par = 1; }
    else if (!strcmp(k, "--scalar")) {
      const char* eq = strchr(v, '=');
      if (eq && plx_opts.n_ov < 8) {
        char* name = (char*)malloc((size_t)(eq - v) + 1);
        memcpy(name, v, (size_t)(eq - v)); name[eq - v] = 0;
        plx_opts.ov_name[plx_opts.n_ov] = name; plx_opts.ov_val[plx_opts.n_ov++] = atof(eq + 1);
      }
      a++;
    } else { fprintf(stderr, "unknown option %s\n", k); return 2; }
  }
  if (plx_opts.reps < 1) plx_opts.reps = 1;
  if (plx_opts.n_threads < 1) plx_default_threads();
  if (!strcmp(cmd, "validate")) return plx_cmd_validate();
  if (!strcmp(cmd, "refute")) return plx_cmd_refute();
  if (!strcmp(cmd, "bench")) return plx_cmd_bench();
  if (!strcmp(cmd, "run")) return plx_cmd_run();
  fprintf(stderr, "unknown command %s\n", cmd);
  return 2;
}
)PLX";

std::string CountExpr(const std::vector<Dim>& dims, const char* prefix) {
  std::ostringstream out;
  for (size_t i = 0; i < dims.size(); ++i) {
    if (i > 0) out << " * ";
    if (dims[i].is_ident) {
      out << "(size_t)" << prefix << dims[i].ident;
    } else {
      out << dims[i].int_value;
    }
  }
  return out.str();
}

const char* FillFunction(TypeBase base) {
  switch (base) {
    case TypeBase::F32: return "plx_fill_float";
    case TypeBase::F64: return "plx_fill_double";
    case TypeBase::I32: return "plx_fill_int";
    case TypeBase::I64: return "plx_fill_ll";
  }
  return "plx_fill_float";
}

bool IsFloat(TypeBase base) { return base == TypeBase::F32 || base == TypeBase::F64; }

// Kernel-specific definitions: dims/args structs, construction, cloning,
// calls, and the result-comparison rule.
std::string Glue(const CodegenResult& gen) {
  std::ostringstream o;
  bool has_par = !gen.par_function_name.empty();
  o << "#define PLX_HAS_PAR " << (has_par ? 1 : 0) << "\n\n";

  o << "typedef struct {";
  if (gen.dim_params.empty()) o << " int plx_none;";
  for (const std::string& d : gen.dim_params) o << " int " << d << ";";
  o << " } PlxDims;\n";
  o << "static PLX_UNUSED void plx_dims(PlxDims* d, int size) {\n";
  if (gen.dim_params.empty()) o << "  (void)size; d->plx_none = 0;\n";
  for (size_t p = 0; p < gen.dim_params.size(); ++p) {
    if (p == 0) {
      o << "  d->" << gen.dim_params[p] << " = size;\n";
    } else {
      o << "  d->" << gen.dim_params[p] << " = size / " << (1 << p) << " + 4;\n";
    }
  }
  o << "}\n";
  o << "static PLX_UNUSED int plx_trip(const PlxDims* d) {\n  (void)d;\n";
  for (const std::string& d : gen.dim_params) o << "  int " << d << " = d->" << d << "; (void)" << d << ";\n";
  o << "  return " << (gen.trip_count_c.empty() ? std::string("0") : gen.trip_count_c) << ";\n}\n\n";

  o << "typedef struct {";
  for (const ArrayParamInfo& a : gen.arrays) o << " " << CTypeName(a.elem_type) << "* " << a.name << ";";
  for (const ScalarParamInfo& s : gen.scalars) o << " " << CTypeName(s.type) << " " << s.name << ";";
  if (gen.arrays.empty() && gen.scalars.empty()) o << " int plx_none;";
  o << " } PlxArgs;\n\n";

  o << "static PLX_UNUSED void plx_make(PlxArgs* a, const PlxDims* d, unsigned seed) {\n  (void)d; (void)seed;\n";
  unsigned idx = 0;
  for (const ArrayParamInfo& arr : gen.arrays) {
    std::string cnt = CountExpr(arr.dims, "d->");
    std::string ct = CTypeName(arr.elem_type);
    if (arr.is_input) {
      o << "  a->" << arr.name << " = (" << ct << "*)malloc((" << cnt << ") * sizeof(" << ct << "));\n"
        << "  " << FillFunction(arr.elem_type) << "(a->" << arr.name << ", " << cnt
        << ", plx_mix(seed * 31u + " << idx << "u));\n";
    } else {
      o << "  a->" << arr.name << " = (" << ct << "*)calloc(" << cnt << ", sizeof(" << ct << "));\n";
    }
    ++idx;
  }
  idx = 0;
  for (const ScalarParamInfo& s : gen.scalars) {
    o << "  a->" << s.name << " = (" << CTypeName(s.type) << ")plx_scalar(\"" << s.name << "\", seed, "
      << idx++ << ", " << (IsFloat(s.type) ? 1 : 0) << ");\n";
  }
  if (gen.arrays.empty() && gen.scalars.empty()) o << "  a->plx_none = 0;\n";
  o << "}\n";

  o << "static PLX_UNUSED void plx_clone(PlxArgs* dst, const PlxArgs* src, const PlxDims* d) {\n  (void)d;\n"
    << "  *dst = *src;\n";
  for (const ArrayParamInfo& arr : gen.arrays) {
    std::string cnt = CountExpr(arr.dims, "d->");
    std::string ct = CTypeName(arr.elem_type);
    o << "  dst->" << arr.name << " = (" << ct << "*)malloc((" << cnt << ") * sizeof(" << ct << "));\n"
      << "  memcpy(dst->" << arr.name << ", src->" << arr.name << ", (" << cnt << ") * sizeof(" << ct
      << "));\n";
  }
  o << "}\n";
  o << "static PLX_UNUSED void plx_free(PlxArgs* a) {\n  (void)a;\n";
  for (const ArrayParamInfo& arr : gen.arrays) o << "  free(a->" << arr.name << ");\n";
  o << "}\n\n";

  auto call = [&](const char* wrapper, const std::string& fn, bool with_order) {
    o << "static PLX_UNUSED PlxRet " << wrapper << "(PlxArgs* a, const PlxDims* d"
      << (with_order ? ", const int* order" : "") << ") {\n  PlxRet r = {0, 0.0};\n  (void)a; (void)d;\n  ";
    std::string args;
    for (const ArgSlot& slot : gen.arg_order) {
      if (!args.empty()) args += ", ";
      args += (slot.kind == ArgSlot::DIM ? "d->" : "a->") + slot.name;
    }
    if (with_order) args += (args.empty() ? "" : ", ") + std::string("order");
    if (gen.return_is_array) {
      o << fn << "(" << args << ");\n";
    } else if (IsFloat(gen.return_base)) {
      o << "r.d = (double)" << fn << "(" << args << ");\n";
    } else {
      o << "r.i = (long long)" << fn << "(" << args << ");\n";
    }
    o << "  return r;\n}\n";
  };
  call("plx_call_seq", gen.seq_function_name, false);
  if (has_par) {
    call("plx_call_par", gen.par_function_name, false);
  } else {
    o << "static PLX_UNUSED PlxRet plx_call_par(PlxArgs* a, const PlxDims* d) { (void)a; (void)d; PlxRet r = {0, 0.0}; return r; }\n";
  }
  call("plx_call_ord", gen.ord_function_name, true);
  o << "\n";

  // Exact for integers, element-wise maps and min/max; relative tolerance only
  // for floating-point + and * reductions, whose rounding legitimately depends
  // on summation order.
  o << "static PLX_UNUSED int plx_equal(const PlxArgs* ref, PlxRet rr, const PlxArgs* got, PlxRet gr,\n"
       "                                const PlxDims* d, long long* idx, double* va, double* vb) {\n"
       "  (void)ref; (void)got; (void)rr; (void)gr; (void)d;\n  *idx = -1; *va = 0; *vb = 0;\n";
  if (gen.return_is_array) {
    const ArrayParamInfo* out = nullptr;
    for (const ArrayParamInfo& a : gen.arrays) {
      if (a.is_output) out = &a;
    }
    o << "  size_t n = " << CountExpr(out->dims, "d->") << ";\n"
      << "  for (size_t k = 0; k < n; k++) {\n"
      << "    if (ref->" << out->name << "[k] != got->" << out->name << "[k]) {\n"
      << "      *idx = (long long)k; *va = (double)ref->" << out->name << "[k]; *vb = (double)got->"
      << out->name << "[k]; return 0;\n    }\n  }\n  return 1;\n";
  } else if (IsFloat(gen.return_base)) {
    if (gen.reduce_op == ReduceOp::MIN || gen.reduce_op == ReduceOp::MAX) {
      o << "  if (rr.d != gr.d) { *va = rr.d; *vb = gr.d; return 0; }\n  return 1;\n";
    } else {
      o << "  double den = fabs(rr.d) > 1e-9 ? fabs(rr.d) : 1e-9;\n"
        << "  if (fabs(rr.d - gr.d) > " << (gen.return_base == TypeBase::F32 ? "1e-4" : "1e-9")
        << " * den) { *va = rr.d; *vb = gr.d; return 0; }\n  return 1;\n";
    }
  } else {
    o << "  if (rr.i != gr.i) { *va = (double)rr.i; *vb = (double)gr.i; return 0; }\n  return 1;\n";
  }
  o << "}\n\n";

  // One-line description of a run's result: the scalar, or a checksum and head of the output array.
  o << "static PLX_UNUSED void plx_summary(const PlxArgs* a, PlxRet r, const PlxDims* d, char* buf, size_t cap) {\n"
       "  (void)a; (void)d; (void)r; (void)cap;\n";
  if (gen.return_is_array) {
    const ArrayParamInfo* out = nullptr;
    for (const ArrayParamInfo& arr : gen.arrays) {
      if (arr.is_output) out = &arr;
    }
    o << "  size_t n = " << CountExpr(out->dims, "d->") << ";\n  double sum = 0;\n"
      << "  for (size_t k = 0; k < n; k++) sum += (double)a->" << out->name << "[k];\n"
      << "  snprintf(buf, cap, \"%s checksum=%.9g first=%.9g\", \"" << out->name << "\", sum, n ? (double)a->" << out->name << "[0] : 0.0);\n";
  } else if (IsFloat(gen.return_base)) {
    o << "  snprintf(buf, cap, \"value=%.12g\", r.d);\n";
  } else {
    o << "  snprintf(buf, cap, \"value=%lld\", r.i);\n";
  }
  o << "}\n";
  return o.str();
}

}  // namespace

HarnessResult GenerateHarness(const KernelDecl& kernel, const PIRKernel& pir,
                              const Schedule& schedule, const CodegenResult& gen) {
  (void)kernel;
  (void)pir;
  HarnessResult result;
  result.attempts_certificate = schedule.kind != ScheduleKind::SEQUENTIAL_ONLY;
  std::ostringstream src;
  // Option parsing / ordering helpers precede the kernel glue (which uses
  // them); the commands that use the glue follow it.
  std::string runtime = kRuntime;
  size_t split = runtime.find("static PLX_UNUSED int plx_cmd_validate");
  src << kPreamble << "\n" << gen.c_source << "\n" << runtime.substr(0, split) << Glue(gen)
      << "\n" << runtime.substr(split);
  result.c_source = src.str();
  return result;
}

}  // namespace parallix
