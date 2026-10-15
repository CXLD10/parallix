#include "parallix/hardware.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>

#include "parallix/compiler_driver.h"

namespace parallix {

namespace {

// "480 KiB (10 instances)" -> per-instance bytes.
long ParseCache(const std::string& value) {
  std::istringstream in(value);
  double n = 0;
  std::string unit;
  in >> n >> unit;
  double mult = unit == "KiB" || unit == "K" ? 1024.0 : unit == "MiB" || unit == "M" ? 1048576.0 : 1.0;
  long inst = 1;
  size_t p = value.find('(');
  if (p != std::string::npos) inst = std::max(1L, std::atol(value.c_str() + p + 1));
  return static_cast<long>(n * mult / static_cast<double>(inst));
}

}  // namespace

HardwareInfo DetectHardware() {
  HardwareInfo hw;
  long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  if (cpus >= 1) {
    hw.logical_cpus = static_cast<int>(cpus);
    hw.source = "sysconf";
  } else {
    hw.fallback = true;
    hw.logical_cpus = 1;
    hw.source = "fallback (core count undetectable, assuming 1)";
  }
#ifdef _SC_LEVEL1_DCACHE_SIZE
  hw.l1d_bytes = sysconf(_SC_LEVEL1_DCACHE_SIZE);
  hw.l2_bytes = sysconf(_SC_LEVEL2_CACHE_SIZE);
  hw.l3_bytes = sysconf(_SC_LEVEL3_CACHE_SIZE);
#endif
  if (hw.l1d_bytes < 0) hw.l1d_bytes = 0;
  if (hw.l2_bytes < 0) hw.l2_bytes = 0;
  if (hw.l3_bytes < 0) hw.l3_bytes = 0;

  std::string out;
  if (RunCommand("lscpu 2>/dev/null", &out) == 0) {
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
      size_t c = line.find(':');
      if (c == std::string::npos) continue;
      std::string key = line.substr(0, c), val = line.substr(c + 1);
      val.erase(0, val.find_first_not_of(" \t"));
      if (key == "Model name") hw.model = val;
      else if (key == "L1d cache" && hw.l1d_bytes == 0) hw.l1d_bytes = ParseCache(val);
      else if (key == "L2 cache" && hw.l2_bytes == 0) hw.l2_bytes = ParseCache(val);
      else if (key == "L3 cache" && hw.l3_bytes == 0) hw.l3_bytes = ParseCache(val);
    }
    hw.source += "+lscpu";
  }
  if (hw.l1d_bytes == 0) hw.l1d_bytes = 32 * 1024;
  if (hw.l2_bytes == 0) hw.l2_bytes = 256 * 1024;
  if (hw.l3_bytes == 0) hw.l3_bytes = 8 * 1024 * 1024;
  return hw;
}

}  // namespace parallix
