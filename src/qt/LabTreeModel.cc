// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/LabTreeModel.h"

#include "qt/BoxServiceClient.h"
#include "qt/LabServiceClient.h"

namespace fmgr::qt {
namespace {

// Recursively build the container subtree rooted at parent_container_id. Each
// container also gets its boxes appended as leaf nodes so a box can be selected
// to scope the sample browser by box_id.
std::vector<TreeNode> buildContainers(BoxServiceClient& boxes,
                                      const QString& token,
                                      const QString& lab_id,
                                      const QString& parent_container_id) {
  std::vector<TreeNode> nodes;
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
    node.children = buildContainers(boxes, token, lab_id, container.id);

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
        freezer_node.children =
            buildContainers(boxes, token, lab.id, freezer.layout_root_id);
        lab_node.children.push_back(std::move(freezer_node));
      }
    }
    roots.push_back(std::move(lab_node));
  }
  return roots;
}

}  // namespace fmgr::qt
