#pragma once

#include <string>
#include <utility>
#include <vector>

namespace parallix {

// A minimal JSON value, write-only: enough to emit certificates and reports.
class Json {
 public:
  enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };

  Json() = default;
  Json(bool b) : type_(BOOL), text_(b ? "true" : "false") {}
  Json(int n) : type_(NUM), text_(std::to_string(n)) {}
  Json(long n) : type_(NUM), text_(std::to_string(n)) {}
  Json(long long n) : type_(NUM), text_(std::to_string(n)) {}
  Json(unsigned n) : type_(NUM), text_(std::to_string(n)) {}
  Json(size_t n) : type_(NUM), text_(std::to_string(n)) {}
  Json(double d);
  Json(const char* s) : type_(STR), text_(s) {}
  Json(std::string s) : type_(STR), text_(std::move(s)) {}

  static Json Array() { Json j; j.type_ = ARR; return j; }
  static Json Object() { Json j; j.type_ = OBJ; return j; }

  Json& Add(Json v) { items_.push_back({"", std::move(v)}); return *this; }                 // array
  Json& Set(const std::string& k, Json v) { items_.push_back({k, std::move(v)}); return *this; }  // object

  std::string Dump(int indent = 0) const;

 private:
  Type type_ = NUL;
  std::string text_;
  std::vector<std::pair<std::string, Json>> items_;
  void DumpTo(std::string& out, int indent, int depth) const;
};

std::string JsonEscape(const std::string& s);

}  // namespace parallix
