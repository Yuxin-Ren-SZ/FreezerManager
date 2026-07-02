// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/LocationPathResolver.h"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <optional>

#include "fmgr/v1/box.grpc.pb.h"
#include "qt/BoxServiceClient.h"

namespace fmgr::qt {
namespace {

QString kindName(v1::ContainerKind kind) {
  switch (kind) {
    case v1::CONTAINER_KIND_RACK:
      return QStringLiteral("Rack");
    case v1::CONTAINER_KIND_SHELF:
      return QStringLiteral("Shelf");
    case v1::CONTAINER_KIND_DRAWER:
      return QStringLiteral("Drawer");
    case v1::CONTAINER_KIND_TOWER:
      return QStringLiteral("Tower");
    case v1::CONTAINER_KIND_GENERIC:
    default:
      return QStringLiteral("Container");
  }
}

// Display label for a container: its human label if set, else its name.
QString containerLabel(const BoxServiceClient::ContainerRow& c) {
  return c.label.isEmpty() ? c.name : c.label;
}

// Load every container in the lab into an id → row map by walking the tree from
// its roots (there is no GetContainer RPC). map.contains() doubles as the BFS
// visited guard against cycles.
QHash<QString, BoxServiceClient::ContainerRow> loadContainers(
    BoxServiceClient* box_client, const QString& session_token,
    const QString& lab_id) {
  QHash<QString, BoxServiceClient::ContainerRow> containers;
  QStringList frontier;
  const auto roots = box_client->listStorageContainers(session_token, lab_id);
  for (const auto& c : roots.containers) {
    if (!containers.contains(c.id)) {
      containers.insert(c.id, c);
      frontier.push_back(c.id);
    }
  }
  while (!frontier.isEmpty()) {
    const QString parent = frontier.takeFirst();
    const auto children =
        box_client->listStorageContainers(session_token, lab_id, parent);
    for (const auto& c : children.containers) {
      if (!containers.contains(c.id)) {
        containers.insert(c.id, c);
        frontier.push_back(c.id);
      }
    }
  }
  return containers;
}

}  // namespace

LocationPathResolver::LocationPathResolver(BoxServiceClient* box_client)
    : box_client_(box_client) {}

LocationPathResolver::Result LocationPathResolver::resolve(
    const QString& session_token, const QString& lab_id, const QString& box_id,
    const QString& position_label) const {
  Result result;

  // Unplaced sample: no box → nothing to resolve.
  if (box_id.isEmpty()) {
    result.ok = true;
    return result;
  }

  const auto box = box_client_->getBox(session_token, box_id);
  if (!box.ok) {
    result.error = box.error.empty() ? std::string("box not found") : box.error;
    return result;
  }

  const auto containers = loadContainers(box_client_, session_token, lab_id);

  // Walk the parent chain from the box's container up to a parentless root.
  QList<BoxServiceClient::ContainerRow> chain;  // box-side first
  QString root_id;
  bool reached_root = false;
  QSet<QString> seen;
  QString cursor = box.box.storage_container_id;
  while (!cursor.isEmpty()) {
    if (seen.contains(cursor)) {  // cycle guard
      result.partial = true;
      break;
    }
    seen.insert(cursor);
    const auto it = containers.constFind(cursor);
    if (it == containers.constEnd()) {  // orphaned / missing parent
      result.partial = true;
      break;
    }
    const BoxServiceClient::ContainerRow& node = it.value();
    chain.push_back(node);
    const std::optional<QString>& parent = node.parent_id;
    if (parent.has_value()) {
      cursor = parent.value();
    } else {
      root_id = node.id;
      reached_root = true;
      cursor.clear();
    }
  }

  // Name the enclosing freezer by matching its layout_root_id to the chain root.
  if (reached_root) {
    const auto freezers = box_client_->listFreezers(session_token, lab_id);
    for (const auto& f : freezers.freezers) {
      if (f.layout_root_id == root_id) {
        result.segments.push_back(
            {PathSegment::Kind::Freezer, f.name, QStringLiteral("Freezer")});
        break;
      }
    }
  }

  // Containers, outermost (root) → innermost (box).
  for (auto it = chain.crbegin(); it != chain.crend(); ++it) {
    result.segments.push_back({PathSegment::Kind::Container,
                               containerLabel(*it), kindName(it->kind)});
  }

  result.segments.push_back(
      {PathSegment::Kind::Box, box.box.label, QStringLiteral("Box")});
  if (!position_label.isEmpty()) {
    result.segments.push_back({PathSegment::Kind::Position, position_label,
                               QStringLiteral("Position")});
  }

  result.ok = true;
  result.placed = true;
  return result;
}

}  // namespace fmgr::qt
