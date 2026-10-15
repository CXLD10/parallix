#pragma once

#include <string>

namespace parallix {

struct HardwareInfo {
  int logical_cpus = 1;
  long l1d_bytes = 0;  // per core, 0 if unknown
  long l2_bytes = 0;
  long l3_bytes = 0;   // shared
  std::string model;
  bool fallback = false;  // true if core count could not be detected
  std::string source;     // where the numbers came from
};

// Core count and cache sizes via sysconf, then lscpu; falls back to a single
// CPU with typical cache sizes if neither works.
HardwareInfo DetectHardware();

}  // namespace parallix
