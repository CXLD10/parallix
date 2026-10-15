#pragma once

#include <string>
#include <vector>

#include "parallix/hardware.h"
#include "parallix/verify.h"

namespace parallix {

struct BenchOptions {
  std::vector<long long> sizes = {100000};
  std::vector<int> threads;  // empty: 1, 2, 4, 8 and the machine maximum
  int reps = 5;              // median of at least 5
  unsigned seed = 1;
  std::string work_dir = "generated";
  std::vector<std::pair<std::string, double>> scalars;
};

struct BenchRow {
  long long size = 0;
  std::string variant;  // "seq" or "par"
  int threads = 1;
  int team = 1;
  int reps = 0;
  double median_s = 0, min_s = 0, max_s = 0;
  bool check_ok = true;
  double speedup = 1;  // seq median / this median
};

struct BenchSizeInfo {
  long long size = 0;
  double model_seq_us = 0;  // cost-model prediction, for comparison
  int model_threads = 1;
  bool model_profitable = false;
  std::string model_reason;
};

struct BenchResult {
  bool ok = false;
  std::string error;
  std::string kernel;
  HardwareInfo hardware;
  std::vector<BenchRow> rows;
  std::vector<BenchSizeInfo> sizes;
};

BenchResult RunBench(const Analysis& a, const BenchOptions& opts, const HardwareInfo& hw);
std::string BenchTable(const BenchResult& r);
std::string BenchCsv(const BenchResult& r);
std::string BenchSvg(const BenchResult& r);

}  // namespace parallix
