// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_SAMPLEBROWSERWIDGET_H
#define FMGR_QT_SAMPLEBROWSERWIDGET_H

#include <optional>

#include <QWidget>

class QTableView;

namespace fmgr::qt {

class SampleServiceClient;
class SampleSearchBar;
class SampleTableModel;

// The authenticated right-hand pane: a SampleSearchBar over a QTableView bound
// to a SampleTableModel. The tree drives setScope(); the search bar layers
// status/barcode filters; the model pages rows in as the view scrolls.
class SampleBrowserWidget : public QWidget {
  Q_OBJECT

 public:
  explicit SampleBrowserWidget(SampleServiceClient* client,
                               QWidget* parent = nullptr);

  void setToken(const QString& session_token);

  // Scope to a lab (and optionally a single box) selected in the tree.
  void setScope(const QString& lab_id,
                const std::optional<QString>& box_id = std::nullopt);

 private:
  SampleTableModel* model_ = nullptr;
  SampleSearchBar* search_bar_ = nullptr;
  QTableView* table_ = nullptr;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_SAMPLEBROWSERWIDGET_H
