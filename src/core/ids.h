// SPDX-License-Identifier: AGPL-3.0-or-later

// Strongly-typed UUID wrappers that prevent ID substitution bugs at compile time.
// Passing a `LabId` where a `UserId` is expected is a compile error, not a runtime
// data corruption. Each `*IdTag` is an empty struct used only as a phantom type
// parameter — it is never instantiated.
// All IDs are UUIDs rather than sequential integers: sequential IDs are enumerable,
// leak record counts to unauthenticated callers, and cannot be merged across deployments.
#ifndef FMGR_CORE_IDS_H
#define FMGR_CORE_IDS_H

#include "core/uuid.h"

#include <nlohmann/json.hpp>

#include <compare>
#include <string>
#include <string_view>

namespace fmgr::core {

  template <typename Tag> class StrongId {
  public:
    constexpr StrongId() = default;
    explicit constexpr StrongId(Uuid value) : value_(value) {}

    [[nodiscard]] static StrongId parse(std::string_view text) {
      return StrongId(Uuid::parse(text));
    }

    [[nodiscard]] std::string to_string() const {
      return value_.to_string();
    }
    [[nodiscard]] constexpr const Uuid& value() const {
      return value_;
    }

    friend constexpr auto operator<=>(const StrongId&, const StrongId&) = default;

  private:
    Uuid value_;
  };

  struct LabIdTag {};
  struct UserIdTag {};
  struct SampleIdTag {};
  struct BoxIdTag {};
  struct BoxTypeIdTag {};
  struct ContainerTypeIdTag {};
  struct StorageContainerIdTag {};
  struct FreezerIdTag {};
  struct ItemTypeIdTag {};
  struct CustomFieldDefinitionIdTag {};
  struct ProjectIdTag {};
  struct CheckoutEventIdTag {};
  struct ShareRequestIdTag {};
  struct RoleIdTag {};
  struct PermissionIdTag {};
  struct AuditEventIdTag {};
  struct SessionIdTag {};
  struct ApiTokenIdTag {};
  struct LoginAttemptIdTag {};

  using LabId = StrongId<LabIdTag>;
  using UserId = StrongId<UserIdTag>;
  using SampleId = StrongId<SampleIdTag>;
  using BoxId = StrongId<BoxIdTag>;
  using BoxTypeId = StrongId<BoxTypeIdTag>;
  using ContainerTypeId = StrongId<ContainerTypeIdTag>;
  using StorageContainerId = StrongId<StorageContainerIdTag>;
  using FreezerId = StrongId<FreezerIdTag>;
  using ItemTypeId = StrongId<ItemTypeIdTag>;
  using CustomFieldDefinitionId = StrongId<CustomFieldDefinitionIdTag>;
  using ProjectId = StrongId<ProjectIdTag>;
  using CheckoutEventId = StrongId<CheckoutEventIdTag>;
  using ShareRequestId = StrongId<ShareRequestIdTag>;
  using RoleId = StrongId<RoleIdTag>;
  using PermissionId = StrongId<PermissionIdTag>;
  using AuditEventId = StrongId<AuditEventIdTag>;
  using SessionId = StrongId<SessionIdTag>;
  using ApiTokenId = StrongId<ApiTokenIdTag>;
  using LoginAttemptId = StrongId<LoginAttemptIdTag>;

  template <typename Tag> void to_json(nlohmann::json& json, const StrongId<Tag>& strong_id) {
    json = strong_id.to_string();
  }

  template <typename Tag> void from_json(const nlohmann::json& json, StrongId<Tag>& strong_id) {
    strong_id = StrongId<Tag>::parse(json.get<std::string>());
  }

} // namespace fmgr::core

#endif // FMGR_CORE_IDS_H
