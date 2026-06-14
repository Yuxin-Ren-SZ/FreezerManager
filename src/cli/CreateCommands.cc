// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/CreateCommands.h"

#include "cli/EntityWrite.h"
#include "core/box.h"
#include "core/freezer.h"
#include "core/item_type.h"
#include "core/timestamp.h"
#include "core/uuid.h"
#include "storage/BoxGeometryTraits.h"
#include "storage/FreezerTraits.h"
#include "storage/ItemTypeTraits.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fmgr::cli {

  namespace {

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      return core::Timestamp::from_unix_micros(static_cast<std::int64_t>(micros));
    }

    [[nodiscard]] storage::MutationContext create_context(const CreateCommon& common) {
      return storage::MutationContext{
          .actor_user_id = common.actor,
          .actor_session_id = "freezerctl-create",
          .request_id = "",
          .reason = "cli_create",
          .lab_id = common.lab_id.to_string(),
      };
    }

    // Uniform grid of positions: row index r → label letter 'A'+r, col index c →
    // 1-based number, every cell accepting the single given size_class. Matches
    // the data/seed/box_types/*.json convention. Capped at 26 rows (single
    // letter); mixed-format boxes use the future JSON-template path.
    [[nodiscard]] std::vector<core::Position> uniform_grid(std::int32_t rows, std::int32_t cols,
                                                           const std::string& size_class) {
      if (rows <= 0 || cols <= 0) {
        throw std::invalid_argument("box-type rows and cols must both be positive");
      }
      if (rows > 26) {
        throw std::invalid_argument("box-type rows must be <= 26 (single-letter row labels)");
      }
      if (size_class.empty()) {
        throw std::invalid_argument("box-type --accepts size_class must not be empty");
      }
      std::vector<core::Position> positions;
      positions.reserve(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols));
      for (std::int32_t row_idx = 0; row_idx < rows; ++row_idx) {
        const char row_letter = static_cast<char>('A' + row_idx);
        for (std::int32_t col_idx = 0; col_idx < cols; ++col_idx) {
          positions.emplace_back(std::string(1, row_letter) + std::to_string(col_idx + 1), row_idx,
                                 col_idx, std::nullopt, std::vector<std::string>{size_class});
        }
      }
      return positions;
    }

  } // namespace

  core::ItemTypeId run_item_type_create(storage::IStorageBackend& backend,
                                        const ItemTypeCreateOptions& options, std::ostream& out) {
    const auto id = core::ItemTypeId::parse(core::generate_uuid_v4());
    const core::ItemType entity{
        .id = id,
        .lab_id = options.common.lab_id,
        .parent_id = options.parent_id,
        .name = options.name,
        .created_at = now_timestamp(),
    };
    insert_in_lab(backend, options.common.lab_id, entity, create_context(options.common));
    out << "created item-type " << id.to_string() << '\n';
    return id;
  }

  core::ContainerTypeId run_container_type_create(storage::IStorageBackend& backend,
                                                  const ContainerTypeCreateOptions& options,
                                                  std::ostream& out) {
    const auto id = core::ContainerTypeId::parse(core::generate_uuid_v4());
    const core::ContainerType entity{
        .id = id,
        .lab_id = options.common.lab_id,
        .name = options.name,
        .size_class = options.size_class,
        .material = options.material,
        .supplier_sku = options.supplier_sku,
        .created_at = now_timestamp(),
    };
    insert_in_lab(backend, options.common.lab_id, entity, create_context(options.common));
    out << "created container-type " << id.to_string() << '\n';
    return id;
  }

  core::StorageContainerId
  run_storage_container_create(storage::IStorageBackend& backend,
                               const StorageContainerCreateOptions& options, std::ostream& out) {
    const auto id = core::StorageContainerId::parse(core::generate_uuid_v4());
    const core::StorageContainer entity{
        .id = id,
        .lab_id = options.common.lab_id,
        .parent_id = options.parent_id,
        .kind = options.kind,
        .name = options.name,
        .label = options.label,
        .ordering_index = options.ordering_index,
        .created_at = now_timestamp(),
    };
    insert_in_lab(backend, options.common.lab_id, entity, create_context(options.common));
    out << "created storage-container " << id.to_string() << '\n';
    return id;
  }

  core::FreezerId run_freezer_create(storage::IStorageBackend& backend,
                                     const FreezerCreateOptions& options, std::ostream& out) {
    const auto id = core::FreezerId::parse(core::generate_uuid_v4());
    const core::Freezer entity{
        .id = id,
        .lab_id = options.common.lab_id,
        .name = options.name,
        .location = options.location,
        .model = options.model,
        .temp_target_c = options.temp_target_c,
        .layout_root_id = options.layout_root_id,
        .created_at = now_timestamp(),
    };
    insert_in_lab(backend, options.common.lab_id, entity, create_context(options.common));
    out << "created freezer " << id.to_string() << '\n';
    return id;
  }

  core::BoxTypeId run_box_type_create(storage::IStorageBackend& backend,
                                      const BoxTypeCreateOptions& options, std::ostream& out) {
    const auto id = core::BoxTypeId::parse(core::generate_uuid_v4());
    const core::BoxType entity{
        .id = id,
        .lab_id = options.common.lab_id,
        .name = options.name,
        .manufacturer = options.manufacturer,
        .sku = options.sku,
        .positions = uniform_grid(options.rows, options.cols, options.accepts_size_class),
        .created_at = now_timestamp(),
    };
    insert_in_lab(backend, options.common.lab_id, entity, create_context(options.common));
    out << "created box-type " << id.to_string() << '\n';
    return id;
  }

  core::BoxId run_box_create(storage::IStorageBackend& backend, const BoxCreateOptions& options,
                             std::ostream& out) {
    const auto id = core::BoxId::parse(core::generate_uuid_v4());
    const core::Box entity{
        .id = id,
        .lab_id = options.common.lab_id,
        .box_type_id = options.box_type_id,
        .storage_container_id = options.storage_container_id,
        .label = options.label,
        .serial = options.serial,
        .barcode = options.barcode,
        .created_at = now_timestamp(),
    };
    insert_in_lab(backend, options.common.lab_id, entity, create_context(options.common));
    out << "created box " << id.to_string() << '\n';
    return id;
  }

} // namespace fmgr::cli
