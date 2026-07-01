// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/LabTreeModel.h"

#include "qt/BoxServiceClient.h"
#include "qt/LabServiceClient.h"

#include <QSet>

namespace fmgr::qt {
namespace {

// Backstop against a malformed container graph: even if a cycle somehow evades
// the path-set guard (e.g. ids that differ on each hop), recursion cannot exceed
// this many levels. A real freezer hierarchy is a handful deep.
constexpr int kMaxContainerDepth = 64;

// Recursively build the container subtree rooted at parent_container_id. Each
// container also gets its boxes appended as leaf nodes so a box can be selected
// to scope the sample browser by box_id.
//
// The storage-container hierarchy is an adjacency list the server does not
// guarantee to be acyclic (a bad parent_id write, or a restore that re-points a
// container at one of its own descendants, could form a cycle). `path` holds the
// container ids on the current branch: if we reach a container already on the
// path we stop descending, so a cyclic graph yields a finite (partial) tree
// instead of unbounded recursion / stack overflow.
std::vector<TreeNode> buildContainers(BoxServiceClient& boxes,
                                      const QString& token,
                                      const QString& lab_id,
                                      const QString& parent_container_id,
                                      QSet<QString>& path, int depth) {
  std::vector<TreeNode> nodes;
  if (depth >= kMaxContainerDepth) {
    return nodes;
  }
  const auto children =
      boxes.listStorageContainers(token, lab_id, parent_container_id);
  if (!children.ok) {
    return nodes;
  }
  for (const auto& container : children.containers) {
    TreeNode node;
    node.kind = QStringLiteral("container");
    node.id = container.id;
    node.lab_id = lab_id;
    node.label = container.label.isEmpty() ? container.name : container.label;
    // Cycle guard: only recurse into a container we have not already visited on
    // this branch. A repeated id means a parent points back into its ancestry;
    // keep the node but do not descend again.
    if (!path.contains(container.id)) {
      path.insert(container.id);
      node.children =
          buildContainers(boxes, token, lab_id, container.id, path, depth + 1);
      path.remove(container.id);
    }

    const auto boxes_in = boxes.listBoxes(token, lab_id, container.id);
    if (boxes_in.ok) {
      for (const auto& box : boxes_in.boxes) {
        TreeNode box_node;
        box_node.kind = QStringLiteral("box");
        box_node.id = box.id;
        box_node.lab_id = lab_id;
        box_node.label = box.label;
        node.children.push_back(std::move(box_node));
      }
    }
    nodes.push_back(std::move(node));
  }
  return nodes;
}

}  // namespace

std::optional<std::vector<TreeNode>> buildLabTree(LabServiceClient& labs,
                                                  BoxServiceClient& boxes,
                                                  const QString& token) {
  const auto labs_result = labs.listLabs(token);
  if (!labs_result.ok) {
    return std::nullopt;
  }

  std::vector<TreeNode> roots;
  for (const auto& lab : labs_result.labs) {
    TreeNode lab_node;
    lab_node.kind = QStringLiteral("lab");
    lab_node.id = lab.id;
    lab_node.lab_id = lab.id;
    lab_node.label = lab.name;

    const auto freezers = boxes.listFreezers(token, lab.id);
    if (freezers.ok) {
      for (const auto& freezer : freezers.freezers) {
        TreeNode freezer_node;
        freezer_node.kind = QStringLiteral("freezer");
        freezer_node.id = freezer.id;
        freezer_node.lab_id = lab.id;
        freezer_node.label = freezer.name;
        // The freezer's layout_root_id is the root container; show its subtree.
        // A fresh path set per freezer root scopes the cycle guard to one branch.
        QSet<QString> path;
        path.insert(freezer.layout_root_id);
        freezer_node.children = buildContainers(
            boxes, token, lab.id, freezer.layout_root_id, path, 0);
        lab_node.children.push_back(std::move(freezer_node));
      }
    }
    roots.push_back(std::move(lab_node));
  }
  return roots;
}

}  // namespace fmgr::qt
