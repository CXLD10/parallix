#pragma once

#include <string>

#include "parallix/hardware.h"
#include "parallix/pipeline.h"
#include "parallix/verify.h"

namespace parallix {

struct CertificateInput {
  std::string source_path;
  std::string source_text;
  const Analysis* analysis = nullptr;
  const ValidationReport* validation = nullptr;  // SAFE / SAFE_AS_REDUCTION kernels
  const RefutationReport* refutation = nullptr;  // UNSAFE kernels
  HardwareInfo hardware;
};

// "CORROBORATED", "MISMATCH", "NOT_VALIDATED", "REFUTED" or "UNSAFE_NOT_REFUTED".
std::string CertificateStatus(const CertificateInput& in);

// The machine-readable certificate (JSON, self-contained: includes the source
// text so `parallix check-cert` can re-verify it with no other input).
std::string BuildCertificateJson(const CertificateInput& in);

}  // namespace parallix
