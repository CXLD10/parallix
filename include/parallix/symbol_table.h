#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "parallix/ast.h"

namespace parallix {

enum class SymbolKind { PARAM, RETURN, DIM, LOOP_INDEX };

// PARAM/RETURN carry a real TypeNode (possibly an array shape). DIM and
// LOOP_INDEX are conceptually scalar i32 constants/indices — `type` is
// left default-constructed (scalar, base unspecified) for those.
struct Symbol {
  SymbolKind kind;
  std::string name;
  TypeNode type;
};

// Holds a kernel's parameter/return/dimension symbols (kernel-scoped, fixed
// once the signature is processed) plus the live stack of loop-index scopes
// pushed and popped as semantic analysis walks the kernel body.
class SymbolTable {
 public:
  // Declares `param` under `kind` (PARAM or RETURN). Any identifier used as
  // an array dimension in its type (e.g. `N` in `f32[N]`) is implicitly
  // declared as a DIM symbol the first time it is seen — the grammar never
  // declares shape parameters separately.
  void DeclareBinding(const Param& param, SymbolKind kind) {
    globals_[param.name] = Symbol{kind, param.name, param.type};
    for (const Dim& dim : param.type.dims) {
      if (dim.is_ident && globals_.find(dim.ident) == globals_.end()) {
        globals_[dim.ident] = Symbol{SymbolKind::DIM, dim.ident, TypeNode{}};
      }
    }
  }

  void PushLoopIndex(const std::string& name) {
    index_stack_.push_back(Symbol{SymbolKind::LOOP_INDEX, name, TypeNode{}});
  }
  void PopLoopIndex() { index_stack_.pop_back(); }

  const Symbol* Lookup(const std::string& name) const {
    // Innermost loop index wins over an (unlikely) same-named global.
    for (auto it = index_stack_.rbegin(); it != index_stack_.rend(); ++it) {
      if (it->name == name) return &*it;
    }
    auto it = globals_.find(name);
    if (it == globals_.end()) return nullptr;
    return &it->second;
  }

  bool IsLoopIndex(const std::string& name) const {
    for (const auto& idx : index_stack_) {
      if (idx.name == name) return true;
    }
    return false;
  }

 private:
  std::unordered_map<std::string, Symbol> globals_;
  std::vector<Symbol> index_stack_;
};

}  // namespace parallix
