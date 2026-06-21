// SPDX-License-Identifier: AGPL-3.0-or-later

// `freezerctl key rotate` — re-wrap every sample's PHI envelope under the KMS's
// active KEK (PRD §8 key rotation). Only the per-record DEK's wrapping changes;
// field ciphertext is untouched, so no plaintext PHI is decrypted to disk. The
// engine is a free function so a future RotateKeys RPC can reuse it. Output is
// written to an injected std::ostream for testability.
#ifndef FMGR_CLI_KEYCOMMANDS_H
#define FMGR_CLI_KEYCOMMANDS_H

#include "core/ids.h"
#include "kms/IKmsProvider.h"
#include "storage/IStorageBackend.h"

#include <cstddef>
#include <optional>
#include <ostream>

namespace fmgr::cli {

  struct RotationReport {
    std::size_t scanned{};   // samples holding a PHI envelope
    std::size_t rewrapped{}; // envelopes moved to the active KEK
    std::size_t current{};   // already on the active KEK (no-op)
    std::size_t failed{};    // could not be re-wrapped (e.g. FK re-validation)

    friend bool operator==(const RotationReport&, const RotationReport&) = default;
  };

  // Re-wrap PHI envelopes to the active KEK across one lab (`lab`) or all labs
  // (nullopt). Each rewrap is audited as a `key.rotate` Sample mutation. Samples
  // that fail to re-wrap are counted in `failed` and logged to `sink` rather than
  // aborting the run, so the old KEK is retired only once `failed == 0` and
  // `rewrapped + current == scanned`.
  RotationReport rotate_phi_keys(storage::IStorageBackend& backend, const kms::IKmsProvider& kms,
                                 std::optional<core::LabId> lab, core::UserId actor,
                                 std::ostream& sink);

} // namespace fmgr::cli

#endif // FMGR_CLI_KEYCOMMANDS_H
