// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_SERVER_PAGETOKEN_H
#define FMGR_SERVER_PAGETOKEN_H

#include <cstdint>
#include <string>
#include <string_view>

namespace fmgr::server {

  struct SamplePageCursor {
    std::int64_t last_modified_at_micros{};
    std::string id;
  };

  [[nodiscard]] std::string encode_sample_page_token(const SamplePageCursor& cursor);
  [[nodiscard]] SamplePageCursor decode_sample_page_token(std::string_view token);

} // namespace fmgr::server

#endif // FMGR_SERVER_PAGETOKEN_H
