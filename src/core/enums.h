// SPDX-License-Identifier: AGPL-3.0-or-later

// Domain enumerations shared across the core model.
// All five enums serialize as snake_case strings (not integers) so audit log
// entries remain human-readable and migrations can add values without renumbering.
#ifndef FMGR_CORE_ENUMS_H
#define FMGR_CORE_ENUMS_H

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fmgr::core {

  enum class SampleStatus : std::uint8_t {
    Active,
    CheckedOut,
    Depleted,
    Destroyed,
    // Soft-delete sentinel. The DB row is retained for audit continuity but
    // hidden from all non-admin queries. Distinct from `Destroyed` (physically
    // consumed or discarded from the freezer).
    Tombstoned,
  };

  enum class CheckoutAction : std::uint8_t {
    CheckedOut,
    CheckedIn,
    // Terminates the sample lifecycle: transitions status to `Destroyed` and
    // consumes any remaining quantity. Irreversible.
    Destroyed,
  };

  enum class RoleKind : std::uint8_t { SystemAdmin, LabAdmin, Member, ReadOnly, ApiClient };

  enum class ContainerKind : std::uint8_t {
    Compartment,
    Shelf,
    Rack,
    Drawer,
    // Lab-defined shape with no structural constraints from the backend. Used
    // for containers that do not map to a standard form factor (e.g. custom
    // foam inserts, dewer drawers). This is the default for new containers.
    Custom,
  };

  enum class UserStatus : std::uint8_t { Active, Disabled };

  [[nodiscard]] inline std::string_view to_string(SampleStatus status) {
    switch (status) {
    case SampleStatus::Active:
      return "active";
    case SampleStatus::CheckedOut:
      return "checked_out";
    case SampleStatus::Depleted:
      return "depleted";
    case SampleStatus::Destroyed:
      return "destroyed";
    case SampleStatus::Tombstoned:
      return "tombstoned";
    }
    throw std::invalid_argument("unknown sample status");
  }

  [[nodiscard]] inline SampleStatus parse_sample_status(std::string_view text) {
    if (text == "active") {
      return SampleStatus::Active;
    }
    if (text == "checked_out") {
      return SampleStatus::CheckedOut;
    }
    if (text == "depleted") {
      return SampleStatus::Depleted;
    }
    if (text == "destroyed") {
      return SampleStatus::Destroyed;
    }
    if (text == "tombstoned") {
      return SampleStatus::Tombstoned;
    }
    throw std::invalid_argument("unknown sample status");
  }

  [[nodiscard]] inline std::string_view to_string(CheckoutAction action) {
    switch (action) {
    case CheckoutAction::CheckedOut:
      return "out";
    case CheckoutAction::CheckedIn:
      return "in";
    case CheckoutAction::Destroyed:
      return "destroy";
    }
    throw std::invalid_argument("unknown checkout action");
  }

  [[nodiscard]] inline CheckoutAction parse_checkout_action(std::string_view text) {
    if (text == "out") {
      return CheckoutAction::CheckedOut;
    }
    if (text == "in") {
      return CheckoutAction::CheckedIn;
    }
    if (text == "destroy") {
      return CheckoutAction::Destroyed;
    }
    throw std::invalid_argument("unknown checkout action");
  }

  [[nodiscard]] inline std::string_view to_string(RoleKind role_kind) {
    switch (role_kind) {
    case RoleKind::SystemAdmin:
      return "system_admin";
    case RoleKind::LabAdmin:
      return "lab_admin";
    case RoleKind::Member:
      return "member";
    case RoleKind::ReadOnly:
      return "read_only";
    case RoleKind::ApiClient:
      return "api_client";
    }
    throw std::invalid_argument("unknown role kind");
  }

  [[nodiscard]] inline RoleKind parse_role_kind(std::string_view text) {
    if (text == "system_admin") {
      return RoleKind::SystemAdmin;
    }
    if (text == "lab_admin") {
      return RoleKind::LabAdmin;
    }
    if (text == "member") {
      return RoleKind::Member;
    }
    if (text == "read_only") {
      return RoleKind::ReadOnly;
    }
    if (text == "api_client") {
      return RoleKind::ApiClient;
    }
    throw std::invalid_argument("unknown role kind");
  }

  [[nodiscard]] inline std::string_view to_string(ContainerKind kind) {
    switch (kind) {
    case ContainerKind::Compartment:
      return "compartment";
    case ContainerKind::Shelf:
      return "shelf";
    case ContainerKind::Rack:
      return "rack";
    case ContainerKind::Drawer:
      return "drawer";
    case ContainerKind::Custom:
      return "custom";
    }
    throw std::invalid_argument("unknown container kind");
  }

  [[nodiscard]] inline std::string_view to_string(UserStatus status) {
    switch (status) {
    case UserStatus::Active:
      return "active";
    case UserStatus::Disabled:
      return "disabled";
    }
    throw std::invalid_argument("unknown user status");
  }

  [[nodiscard]] inline UserStatus parse_user_status(std::string_view text) {
    if (text == "active") {
      return UserStatus::Active;
    }
    if (text == "disabled") {
      return UserStatus::Disabled;
    }
    throw std::invalid_argument("unknown user status");
  }

  [[nodiscard]] inline ContainerKind parse_container_kind(std::string_view text) {
    if (text == "compartment") {
      return ContainerKind::Compartment;
    }
    if (text == "shelf") {
      return ContainerKind::Shelf;
    }
    if (text == "rack") {
      return ContainerKind::Rack;
    }
    if (text == "drawer") {
      return ContainerKind::Drawer;
    }
    if (text == "custom") {
      return ContainerKind::Custom;
    }
    throw std::invalid_argument("unknown container kind");
  }

  inline void to_json(nlohmann::json& json, SampleStatus status) {
    json = std::string(to_string(status));
  }
  inline void from_json(const nlohmann::json& json, SampleStatus& status) {
    status = parse_sample_status(json.get<std::string>());
  }

  inline void to_json(nlohmann::json& json, CheckoutAction action) {
    json = std::string(to_string(action));
  }
  inline void from_json(const nlohmann::json& json, CheckoutAction& action) {
    action = parse_checkout_action(json.get<std::string>());
  }

  inline void to_json(nlohmann::json& json, RoleKind role_kind) {
    json = std::string(to_string(role_kind));
  }
  inline void from_json(const nlohmann::json& json, RoleKind& role_kind) {
    role_kind = parse_role_kind(json.get<std::string>());
  }

  inline void to_json(nlohmann::json& json, ContainerKind kind) {
    json = std::string(to_string(kind));
  }
  inline void from_json(const nlohmann::json& json, ContainerKind& kind) {
    kind = parse_container_kind(json.get<std::string>());
  }

  inline void to_json(nlohmann::json& json, UserStatus status) {
    json = std::string(to_string(status));
  }
  inline void from_json(const nlohmann::json& json, UserStatus& status) {
    status = parse_user_status(json.get<std::string>());
  }

} // namespace fmgr::core

#endif // FMGR_CORE_ENUMS_H
