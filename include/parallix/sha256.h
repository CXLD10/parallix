#pragma once

#include <string>

namespace parallix {

// SHA-256 of `data`, as 64 lowercase hex characters.
std::string Sha256Hex(const std::string& data);

}  // namespace parallix
