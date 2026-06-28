// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/LabTreeWidget.h"

#include <QStringList>

#include "qt/LabTreeModel.h"

namespace fmgr::qt {

LabTreeWidget::LabTreeWidget(LabServiceClient* labs, BoxServiceClient* boxes,
                             QWidget* parent)
    : QTreeWidget(parent), labs_(labs), boxes_(boxes) {
  setHeaderLabels(QStringList{QStringLiteral("Labs / Freezers / Containers")});
  connect(this, &QTreeWidget::itemClicked, this,
          &LabTreeWidget::handleItemActivated);
}

bool LabTreeWidget::reload() {
  clear();

  const auto tree = buildLabTree(*labs_, *boxes_, token_);
  if (!tree.has_value()) {
    return false;
  }
  for (const TreeNode& node : *tree) {
    appendNode(nullptr, node);
  }
  return true;
}

void LabTreeWidget::appendNode(QTreeWidgetItem* parent_item,
                               const TreeNode& node) {
  auto* item = parent_item == nullptr ? new QTreeWidgetItem(this)
                                      : new QTreeWidgetItem(parent_item);
  item->setText(0, node.label);
  item->setData(0, kKindRole, node.kind);
  item->setData(0, kIdRole, node.id);
  for (const TreeNode& child : node.children) {
    appendNode(item, child);
  }
}

void LabTreeWidget::handleItemActivated(QTreeWidgetItem* item, int /*column*/) {
  if (item == nullptr) {
    return;
  }
  const QString kind = item->data(0, kKindRole).toString();
  const QString id = item->data(0, kIdRole).toString();
  emit nodeSelected(kind, id);
}

}  // namespace fmgr::qt
