// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_LABTREEWIDGET_H
#define FMGR_QT_LABTREEWIDGET_H

#include <QString>
#include <QTreeWidget>

namespace fmgr::qt {

class LabServiceClient;
class BoxServiceClient;
struct TreeNode;

// Navigation tree for the authenticated app: Labs → Freezers → StorageContainers.
//
// The two service clients are injected (non-owning) so the widget can be driven
// by a real channel in production or in-process fakes in tests — the same seam
// AuthServiceClient/SessionManager use. The bearer token is supplied via
// setToken() before reload(), which (re)builds the whole tree eagerly.
//
// The data tree is produced headlessly by buildLabTree() (see LabTreeModel.h),
// which is where the logic is unit-tested; this widget is thin glue that renders
// those nodes and emits nodeSelected(kind, id) on click so later modules (e.g.
// the sample browser) can scope their queries to the clicked entity.
class LabTreeWidget : public QTreeWidget {
  Q_OBJECT

 public:
  // Roles used to stash the entity kind ("lab"/"freezer"/"container"/"box"), id,
  // and owning lab id on each tree item.
  static constexpr int kKindRole = ::Qt::UserRole;
  static constexpr int kIdRole = ::Qt::UserRole + 1;
  static constexpr int kLabIdRole = ::Qt::UserRole + 2;

  LabTreeWidget(LabServiceClient* labs, BoxServiceClient* boxes,
                QWidget* parent = nullptr);

  void setToken(const QString& session_token) { token_ = session_token; }

  // Rebuild the tree from the server. Returns false (and leaves the tree empty)
  // if the labs listing fails.
  bool reload();

 signals:
  void nodeSelected(const QString& kind, const QString& id,
                    const QString& lab_id);

 private slots:
  // Wired to itemClicked; emits nodeSelected from the item's stored data.
  void handleItemActivated(QTreeWidgetItem* item, int column);

 private:
  void appendNode(QTreeWidgetItem* parent_item, const TreeNode& node);

  LabServiceClient* labs_;
  BoxServiceClient* boxes_;
  QString token_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_LABTREEWIDGET_H
