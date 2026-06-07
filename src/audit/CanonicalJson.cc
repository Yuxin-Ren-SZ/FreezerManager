// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audit/CanonicalJson.h"

#include <sodium.h>

#include <array>
#include <stdexcept>
#include <string>

namespace fmgr::audit {

  std::string canonical_json(const nlohmann::json& value) {
    // nlohmann::json default type uses std::map for objects → keys are sorted.
    // dump(-1) = compact, no indent.  ensure_ascii=false preserves UTF-8.
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
  }

  std::string compute_audit_hash(std::string_view prev_hash, std::string_view content_json) {
    if (sodium_init() < 0) {
      throw std::runtime_error("libsodium initialisation failed");
    }
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    crypto_hash_sha256_update(&state, reinterpret_cast<const unsigned char*>(prev_hash.data()),
                              prev_hash.size());
    crypto_hash_sha256_update(&state, reinterpret_cast<const unsigned char*>(content_json.data()),
                              content_json.size());
    std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
    crypto_hash_sha256_final(&state, hash.data());

    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
    return {hex.data()};
  }

} // namespace fmgr::audit
