#pragma once

#include <string>
#include <vector>

namespace parallix {

struct CertCheckResult {
  bool ok = true;
  std::vector<std::string> passed;    // checks that held
  std::vector<std::string> failures;  // checks that failed (non-empty => ok == false)
  std::vector<std::string> notes;     // things the checker does NOT re-verify
};

// Independent certificate checker. It shares no code with the dependence
// engine: it has its own JSON reader and its own tiny expression evaluator, and
// only re-derives claims from the data stored in the certificate (the source
// text, loop bounds, subscripts and the named iterations).
// `external_source`, if non-null, is the on-disk source text to compare against.
CertCheckResult CheckCertificate(const std::string& json_text, const std::string* external_source);

}  // namespace parallix
