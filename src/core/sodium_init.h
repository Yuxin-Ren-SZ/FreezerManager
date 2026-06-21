// SPDX-License-Identifier: AGPL-3.0-or-later

// Shared libsodium initialization guard. sodium_init() is idempotent and
// thread-safe after the first successful call and returns >= 0 on success.
// Centralised here so the init check is not copy-pasted across the crypto and
// kms modules (review N-2). Callers throw their own module-specific error
// (CipherError / KmsError) on a false result so existing catch-by-type
// contracts are preserved.
#ifndef FMGR_CORE_SODIUM_INIT_H
#define FMGR_CORE_SODIUM_INIT_H

#include <sodium.h>

namespace fmgr::core {

  [[nodiscard]] inline bool sodium_ready() {
    return sodium_init() >= 0;
  }

} // namespace fmgr::core

#endif // FMGR_CORE_SODIUM_INIT_H
