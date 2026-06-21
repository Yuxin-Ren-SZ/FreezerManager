// SPDX-License-Identifier: AGPL-3.0-or-later

#include "kms/OsKeyringKms.h"

#include <sodium.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace fmgr::kms {

  namespace {

    constexpr const char* k_default_basename = "master_kek";

    std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path) {
      std::ifstream stream(path, std::ios::binary);
      if (!stream) {
        throw KmsError("cannot read KEK file: " + path.string());
      }
      return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

    // A KEK file is either raw 32 bytes or base64 of 32 bytes (with optional
    // trailing whitespace/newline). Normalise to raw 32 bytes.
    std::vector<std::uint8_t> load_kek(const std::filesystem::path& path) {
      std::vector<std::uint8_t> raw = read_file_bytes(path);
      if (raw.size() == crypto_secretbox_KEYBYTES) {
        return raw; // raw binary key
      }
      const std::string text(raw.begin(), raw.end());
      std::vector<std::uint8_t> kek(crypto_secretbox_KEYBYTES);
      std::size_t decoded_len = 0;
      if (sodium_base642bin(kek.data(), kek.size(), text.data(), text.size(), " \n\r\t",
                            &decoded_len, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0 ||
          decoded_len != crypto_secretbox_KEYBYTES) {
        throw KmsError("KEK file is neither raw 32 bytes nor base64 of 32 bytes: " + path.string());
      }
      return kek;
    }

  } // namespace

  OsKeyringKms::OsKeyringKms(std::vector<std::uint8_t> active,
                             std::vector<std::vector<std::uint8_t>> retired)
      : KeyringKms(std::move(active), std::move(retired)) {}

  OsKeyringKms OsKeyringKms::from_credentials_dir(const std::filesystem::path& dir) {
    return from_credentials_dir(dir, k_default_basename);
  }

  OsKeyringKms OsKeyringKms::from_credentials_dir(const std::filesystem::path& dir,
                                                  const std::string& basename) {
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) {
      throw KmsError("credential directory does not exist: " + dir.string());
    }
    const std::filesystem::path active_path = dir / basename;
    if (!std::filesystem::exists(active_path, error)) {
      throw KmsError("active KEK file is missing: " + active_path.string());
    }
    std::vector<std::uint8_t> active = load_kek(active_path);

    // Collect retired keys in a deterministic order (sorted by filename).
    const std::string retired_prefix = basename + ".prev.";
    std::vector<std::filesystem::path> retired_paths;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
      const std::string name = entry.path().filename().string();
      if (name.rfind(retired_prefix, 0) == 0) {
        retired_paths.push_back(entry.path());
      }
    }
    std::sort(retired_paths.begin(), retired_paths.end());

    std::vector<std::vector<std::uint8_t>> retired;
    retired.reserve(retired_paths.size());
    for (const auto& path : retired_paths) {
      retired.push_back(load_kek(path));
    }
    return OsKeyringKms(std::move(active), std::move(retired));
  }

  OsKeyringKms OsKeyringKms::from_systemd_credentials() {
    return from_systemd_credentials(k_default_basename);
  }

  OsKeyringKms OsKeyringKms::from_systemd_credentials(const std::string& basename) {
    const char* dir = std::getenv("CREDENTIALS_DIRECTORY"); // NOLINT(concurrency-mt-unsafe)
    if (dir == nullptr || *dir == '\0') {
      throw KmsError("CREDENTIALS_DIRECTORY is not set");
    }
    return from_credentials_dir(std::filesystem::path(dir), basename);
  }

} // namespace fmgr::kms
