// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_BOXSERVICECLIENT_H
#define FMGR_QT_BOXSERVICECLIENT_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QString>

#include "fmgr/v1/box.grpc.pb.h"

namespace fmgr::qt {

// Synchronous, UI-agnostic wrapper over BoxService's layout read RPCs
// (ListFreezers, ListStorageContainers). Mirrors AuthServiceClient: owns a stub
// interface, never throws, and attaches the bearer token on every call. Used by
// LabTreeWidget to build the Labs → Freezers → Containers hierarchy.
class BoxServiceClient {
 public:
  struct FreezerRow {
    QString id;
    QString lab_id;
    QString name;
    QString location;
    QString model;
    QString layout_root_id;
  };

  struct ContainerRow {
    QString id;
    QString lab_id;
    std::optional<QString> parent_id;
    v1::ContainerKind kind = v1::CONTAINER_KIND_UNSPECIFIED;
    QString name;
    QString label;
  };

  struct BoxRow {
    QString id;
    QString lab_id;
    QString box_type_id;
    QString storage_container_id;
    QString label;
  };

  struct PositionRow {
    QString label;
    int row = 0;
    int col = 0;
    std::optional<int> z;
    std::vector<QString> accepts;
  };

  struct BoxTypeRow {
    QString id;
    QString name;
    std::vector<PositionRow> positions;
  };

  struct ListFreezersResult {
    bool ok = false;
    std::string error;
    std::vector<FreezerRow> freezers;
  };

  struct ListContainersResult {
    bool ok = false;
    std::string error;
    std::vector<ContainerRow> containers;
  };

  struct ListBoxesResult {
    bool ok = false;
    std::string error;
    std::vector<BoxRow> boxes;
  };

  struct GetBoxResult {
    bool ok = false;
    std::string error;
    BoxRow box;
  };

  struct ListBoxTypesResult {
    bool ok = false;
    std::string error;
    std::vector<BoxTypeRow> box_types;
  };

  explicit BoxServiceClient(std::unique_ptr<v1::BoxService::StubInterface> stub);

  // List the freezers in a lab.
  ListFreezersResult listFreezers(const QString& session_token,
                                  const QString& lab_id);

  // List storage containers in a lab. When parent_id is set, only the direct
  // children of that container are returned; when unset, the top-level
  // (parentless) containers are returned.
  ListContainersResult listStorageContainers(
      const QString& session_token, const QString& lab_id,
      const std::optional<QString>& parent_id = std::nullopt);

  // List the boxes placed directly in a storage container.
  ListBoxesResult listBoxes(const QString& session_token, const QString& lab_id,
                            const QString& storage_container_id);

  // Fetch a single box (carries its box_type_id).
  GetBoxResult getBox(const QString& session_token, const QString& box_id);

  // List the lab's box types, each with its position grid. There is no
  // GetBoxType RPC, so callers match on box_type_id themselves.
  ListBoxTypesResult listBoxTypes(const QString& session_token,
                                  const QString& lab_id);

 private:
  std::unique_ptr<v1::BoxService::StubInterface> stub_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_BOXSERVICECLIENT_H
