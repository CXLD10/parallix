#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "parallix/dependence.h"
#include "parallix/ir.h"

namespace parallix {

// A concrete loop-carried conflict: two iterations of the outermost loop whose
// references touch the same array element.
struct Witness {
  bool found = false;
  std::string note;  // why nothing was found, when !found

  size_t pair_index = 0;                 // index into DependenceReport::pairs
  size_t first_ref = 0, second_ref = 0;  // PIR reference indices; "first" runs earlier sequentially
  std::vector<std::pair<std::string, long long>> first_iter, second_iter;  // loop var -> value
  std::map<std::string, long long> dims;  // concrete dimension values used
  std::string array;
  std::vector<long long> element;  // the shared element's coordinates
  DependenceType type = DependenceType::NONE;  // by sequential order: RAW / WAR / WAW
  std::string explanation;
};

// Dimension values the harness uses for a given problem size: the first symbol
// gets `size`, symbol p>0 gets size / 2^p + 4.
std::map<std::string, long long> DimsForSize(const std::vector<std::string>& dim_params, long long size);

// For every dependent pair in `report`, searches the iteration space for a
// concrete witness at the given dims. The search is exhaustive over a bounded
// iteration space, so it is independent of the distance arithmetic in the
// dependence engine.
std::vector<Witness> FindWitnesses(const PIRKernel& kernel, const DependenceReport& report,
                                   const std::vector<std::string>& dim_params, long long size);

}  // namespace parallix
