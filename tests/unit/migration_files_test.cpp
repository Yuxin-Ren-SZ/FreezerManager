// SPDX-License-Identifier: AGPL-3.0-or-later

// Guards the build-time-embedded migrations (review C8). The critical invariant:
// moving migration SQL out of the backend .cc into .sql files must NOT change any
// migration checksum — an already-applied migration in a deployed database stores
// blake2b("version:name:up_sql") and aborts on mismatch. The golden hashes below
// are pinned to the values computed before the split, so any byte drift fails
// here. Also checks ordering and that the embedded bodies match the .sql files.

#include "storage/EmbeddedMigration.h"
#include "storage/postgres/PostgresMigrationsEmbedded.h"
#include "storage/sqlite/SqliteMigrationsEmbedded.h"

#include <sodium.h>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

  std::string blake2b_hex(const std::string& data) {
    if (sodium_init() < 0) {
      throw std::runtime_error("libsodium init failed");
    }
    std::array<unsigned char, crypto_generichash_BYTES> hash{};
    crypto_generichash(hash.data(), hash.size(),
                       reinterpret_cast<const unsigned char*>(data.data()), data.size(), nullptr,
                       0);
    static constexpr char k[] = "0123456789abcdef";
    std::string hex;
    for (unsigned char b : hash) {
      hex.push_back(k[b >> 4]);
      hex.push_back(k[b & 0xF]);
    }
    return hex;
  }

  std::string checksum(const fmgr::storage::EmbeddedMigration& m) {
    return blake2b_hex(std::to_string(m.version) + ":" + std::string(m.name) + ":" +
                       std::string(m.up_sql));
  }

  const std::map<std::string, std::string>& sqlite_golden() {
    static const std::map<std::string, std::string> g{
        {"0001_init", "db4814f4a5c121e33ee7459d2b22ba8e53bd8627383154ca6a502252bf9e0d1d"},
        {"0002_identity", "794be1a6971d28bbb6fa2de44d35665b336bd6ba3a59f007569179723a4baf41"},
        {"0003_roles", "f6eba3ffaff20378cdf33ced63980b96c8c70733d646e5f2c452506a7ddd28dc"},
        {"0004_layout", "2ea902e08cbbbc00ed495b7df56cced654eb0adfd2c931f15f3341c9f5d806d9"},
        {"0005_box_types", "b28d7074da6ccac922f6f15307a457be9477934f2b4f53be92100c3724a95c1b"},
        {"0006_boxes", "c2b426b90851e911aea9ea46caafc9a0accd5f6f6bf0a90534fe4501bb89b63f"},
        {"0007_item_types", "3d916e7dedf53b7abc404b7a12991e62ca5d3f5ce3af3f6eb08e202ab4d39ecb"},
        {"0008_samples", "fc14c3c65c353c4ba25a6eeadc8c170163fb1aabf914510965ec6df147d6bf64"},
        {"0009_share_requests", "d78a9fcbe79cd743ee9a3c29ddecb0556f0f339c4fcd727f54d45726c26405d2"},
        {"0010_sessions", "7eb9ad51b856ea164f1df6bc349f86307e94daaabc24f97578d45c758a7798bc"},
        {"0011_audit_events", "dabbc8546c9e83b500ced837eeeb87eef1007bf2195b279b9142f2def8c9019f"},
        {"0012_sessions_mfa", "07e61db9c1347ec1468bfbb0e69ee13da936fcd14596a7924974b8c0c82a3bd1"},
        {"0013_authz_version", "685934d6d8b2a5be81b097aa54f4a4b4223acec106d2c094fa1c479bb8ca2d60"},
        {"0014_lab_provision", "1a6c4c4406ac50d6bc06e2863522dc89a4e70827162b7273026d8fd7a62b2e60"},
        {"0015_login_attempt", "0d30ca7be5d39941bac4acf53dccda958d848cdc7b5335940f7f7a60a2ec02d8"},
    };
    return g;
  }

  const std::map<std::string, std::string>& postgres_golden() {
    static const std::map<std::string, std::string> g{
        {"0001_init", "c374d5af7204012b39e01411881bd9d39565abbda80d4a2ddd0194a6bcd87b8f"},
        {"0002_identity", "d733de7ff749f474f844e2203290ca4343f65c50915297ea6acfd099c48ab673"},
        {"0003_roles", "7f1433419a2fe3f5a579d165b7ebe00bb4d3f3d09ca29bb6e5ed0ac7ae309cc8"},
        {"0004_layout", "f2da63c74d4ee58c736af5f5d0017f9f7e8d07bd4f5f052f9cb935133c5fca89"},
        {"0005_box_types", "8091f10224942c1b1f7dcf34f682c15c6b2afbf595e05a5caf2fbfe0acad4980"},
        {"0006_boxes", "79c0d2daa1fe8be048c2079a3903f17cc3f9c15e5263a404bea032e0570bd4bb"},
        {"0007_item_types", "a8c36c8e62ba0a1ff9df25d9b8d991a68708fa03ddb4084bc54a58ac8ed600fd"},
        {"0008_samples", "ae5cacc180a90e16a2645432ea734a65e16daaeaf2c31ec65480a86d68ad6fe3"},
        {"0009_share_requests", "96280bf2b300cc623bb432cb5635bf219220c46f38c6fdadb9a5d9fbb847ba93"},
        {"0010_sessions", "c46510c7e339060f6932019ab49e87476664a0404dbe83141b0d83ba005931e6"},
        {"0011_audit_events_full", "a55fad9396fcf8a4fcda28f088429d5d9009148a1272c4bce1392be063169053"},
        {"0012_sessions_mfa", "e74198fa039a6b101251641ea355150f636f80bcb365b08a2bc6c66c479f9523"},
        {"0013_authz_version", "dd3d82f5bcbcb03dfd5c2f15db84b29a24f60242bd1e485580cb9b68ff0f0220"},
        {"0014_lab_provision", "238d806d5f90513df81df86cf0f9bb04bea868e6a1e6034ec5ead490a93d8493"},
        {"0015_login_attempt", "90e76c0314db4a006ac3796cc8175c5093b98dc0bc8d193794170622b527fd0f"},
    };
    return g;
  }

  std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  template <typename Arr>
  void check_checksums(const Arr& migrations, const std::map<std::string, std::string>& golden) {
    ASSERT_EQ(migrations.size(), golden.size());
    for (const auto& m : migrations) {
      const std::string name(m.name);
      const auto it = golden.find(name);
      ASSERT_NE(it, golden.end()) << "unexpected migration: " << name;
      EXPECT_EQ(checksum(m), it->second)
          << "checksum drift for " << name << " — an applied migration would fail to verify";
    }
  }

  template <typename Arr> void check_ordering(const Arr& migrations) {
    int prev = 0;
    for (const auto& m : migrations) {
      EXPECT_GT(m.version, prev) << "versions must be strictly increasing (array is sorted)";
      prev = m.version;
      const std::string name(m.name);
      ASSERT_GE(name.size(), 4U);
      EXPECT_EQ(std::stoi(name.substr(0, 4)), m.version)
          << "filename prefix must match version for " << name;
    }
  }

  template <typename Arr>
  void check_sql_files_match(const Arr& migrations, const std::filesystem::path& dir) {
    for (const auto& m : migrations) {
      const auto path = dir / (std::string(m.name) + ".sql");
      ASSERT_TRUE(std::filesystem::exists(path)) << "missing .sql file: " << path;
      EXPECT_EQ(read_file(path), std::string(m.up_sql))
          << "embedded body out of sync with .sql — regenerate the header for " << m.name;
    }
  }

  TEST(MigrationFiles, SqliteChecksumsAreStable) {
    check_checksums(fmgr::storage::k_sqlite_embedded_migrations, sqlite_golden());
  }

  TEST(MigrationFiles, PostgresChecksumsAreStable) {
    check_checksums(fmgr::storage::k_postgres_embedded_migrations, postgres_golden());
  }

  TEST(MigrationFiles, SqliteVersionsOrdered) {
    check_ordering(fmgr::storage::k_sqlite_embedded_migrations);
  }

  TEST(MigrationFiles, PostgresVersionsOrdered) {
    check_ordering(fmgr::storage::k_postgres_embedded_migrations);
  }

  TEST(MigrationFiles, SqliteEmbeddedMatchesSqlFiles) {
    check_sql_files_match(fmgr::storage::k_sqlite_embedded_migrations,
                          FMGR_SQLITE_MIGRATIONS_DIR);
  }

  TEST(MigrationFiles, PostgresEmbeddedMatchesSqlFiles) {
    check_sql_files_match(fmgr::storage::k_postgres_embedded_migrations,
                          FMGR_PG_MIGRATIONS_DIR);
  }

} // namespace
