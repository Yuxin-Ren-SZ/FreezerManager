// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_LABTREEMODEL_H
#define FMGR_QT_LABTREEMODEL_H

#include <optional>
#include <vector>

#include <QString>

namespace fmgr::qt {

class LabServiceClient;
class BoxServiceClient;

// One node in the navigation tree. kind is "lab" | "freezer" | "container";
// id is the entity id (stashed on the rendered QTreeWidgetItem); label is the
// display text. This is a plain data tree with no Qt-widget dependency, so the
// build logic is unit-testable headlessly (no QApplication needed).
struct TreeNode {
  QString kind;
  QString id;
  QString label;
  std::vector<TreeNode> children;
};

// Build the Labs → Freezers → Containers tree by walking the service clients
// with the given bearer token. Returns std::nullopt if the top-level labs
// listing fails; failures lower in the tree are skipped (that subtree is left
// empty) rather than aborting the whole build.
std::optional<std::vector<TreeNode>> buildLabTree(LabServiceClient& labs,
                                                  BoxServiceClient& boxes,
                                                  const QString& token);

}  // namespace fmgr::qt

#endif  // FMGR_QT_LABTREEMODEL_H
