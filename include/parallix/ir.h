#pragma once

#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "parallix/ast.h"

namespace parallix {

// A linear combination of named symbols (enclosing loop indices, or
// compile-time-known dimension parameters like `N`) plus a constant:
// sum(coefficient * name) + constant. Used for loop bounds (specs/03).
struct AffineExpr {
  std::vector<std::pair<std::string, long long>> terms;  // name -> nonzero coefficient
  long long constant = 0;

  std::string ToString() const {
    std::ostringstream out;
    bool first = true;
    for (const auto& [name, coeff] : terms) {
      out << (first ? (coeff < 0 ? "-" : "") : (coeff < 0 ? " - " : " + "));
      long long mag = coeff < 0 ? -coeff : coeff;
      if (mag != 1) out << mag << "*";
      out << name;
      first = false;
    }
    if (constant != 0 || first) {
      long long mag = constant < 0 ? -constant : constant;
      out << (first ? (constant < 0 ? "-" : "") : (constant < 0 ? " - " : " + ")) << mag;
    }
    return out.str();
  }
};

enum class RefKind { READ, WRITE };

inline const char* ToString(RefKind kind) { return kind == RefKind::READ ? "read" : "write"; }

struct SourceLoc {
  int line = 0;
  int column = 0;
};

// A single array dimension's access function: coefficient*index_var +
// constant (specs/03, specs/04). index_var is empty (coefficient always 0)
// when that dimension's subscript does not depend on any loop index.
struct AccessFunction {
  std::string index_var;
  long long coefficient = 0;
  long long constant = 0;
};

// One array read or write inside the innermost loop body.
struct Reference {
  std::string array_name;
  RefKind kind;
  std::vector<AccessFunction> access;  // one entry per array dimension, in order
  SourceLoc loc;
};

// One level of the loop nest, outer to inner. A `reduce(op) { }` header also
// contributes a level here (it is a loop with a sequential carried
// dependency by construction) — see PIRKernel::is_reduction for the rest of
// the reduction-specific metadata.
struct LoopLevel {
  std::string index_var;
  AffineExpr lower;
  AffineExpr upper;
  LoopMode mode;
};

// The whole PIR for one kernel: specs/03's loop nest + references +
// (implicitly) the iteration domain, which is just [lower, upper) per level.
struct PIRKernel {
  std::string name;
  std::vector<LoopLevel> loop_nest;   // outer to inner
  std::vector<Reference> references;  // every array read/write in the innermost body

  bool is_reduction = false;
  ReduceOp reduce_op = ReduceOp::ADD;
  std::string reduce_accumulator;  // scalar variable name; valid iff is_reduction
};

}  // namespace parallix
