#include "parallix/json.h"

#include <cmath>
#include <cstdio>

namespace parallix {

Json::Json(double d) : type_(NUM) {
  if (!std::isfinite(d)) {
    type_ = NUL;
    return;
  }
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.12g", d);
  text_ = buf;
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

void Json::DumpTo(std::string& out, int indent, int depth) const {
  auto nl = [&](int d) {
    if (indent > 0) out += "\n" + std::string(static_cast<size_t>(indent * d), ' ');
  };
  switch (type_) {
    case NUL: out += "null"; return;
    case BOOL: case NUM: out += text_; return;
    case STR: out += "\"" + JsonEscape(text_) + "\""; return;
    case ARR:
      if (items_.empty()) { out += "[]"; return; }
      out += "[";
      for (size_t i = 0; i < items_.size(); ++i) {
        if (i) out += ",";
        nl(depth + 1);
        items_[i].second.DumpTo(out, indent, depth + 1);
      }
      nl(depth);
      out += "]";
      return;
    case OBJ:
      if (items_.empty()) { out += "{}"; return; }
      out += "{";
      for (size_t i = 0; i < items_.size(); ++i) {
        if (i) out += ",";
        nl(depth + 1);
        out += "\"" + JsonEscape(items_[i].first) + "\":" + (indent > 0 ? " " : "");
        items_[i].second.DumpTo(out, indent, depth + 1);
      }
      nl(depth);
      out += "}";
      return;
  }
}

std::string Json::Dump(int indent) const {
  std::string out;
  DumpTo(out, indent, 0);
  return out;
}

}  // namespace parallix
