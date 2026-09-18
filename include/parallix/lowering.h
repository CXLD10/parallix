#pragma once

#include <stdexcept>
#include <string>

#include "parallix/ast.h"
#include "parallix/ir.h"

namespace parallix {

// Thrown when a construct is outside Phase 1's PIR scope (constitution rule
// 6), e.g. a single array dimension's subscript depending on more than one
// distinct loop index. Sema already guarantees affineness; this is a
// narrower, Phase-1-specific restriction on top of that.
class LoweringError : public std::runtime_error {
 public:
  explicit LoweringError(const std::string& message) : std::runtime_error(message) {}
};

// Lowers a semantically-valid kernel (i.e. one that already passed Sema) to
// PIR per specs/03.
PIRKernel LowerKernel(const KernelDecl& kernel);

}  // namespace parallix
