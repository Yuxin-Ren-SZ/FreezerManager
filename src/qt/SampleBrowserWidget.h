// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_SAMPLEBROWSERWIDGET_H
#define FMGR_QT_SAMPLEBROWSERWIDGET_H

#include <optional>

#include <QWidget>

class QTableView;
class QTimer;

namespace fmgr::qt {

class SampleServiceClient;
class SampleSearchBar;
class SampleTableModel;
class SampleWatchSubscriber;

// The authenticated right-hand pane: a SampleSearchBar over a QTableView bound
// to a SampleTableModel. The tree drives setScope(); the search bar layers
// status/barcode filters; the model pages rows in as the view scrolls.
class SampleBrowserWidget : public QWidget {
  Q_OBJECT

 public:
  explicit SampleBrowserWidget(SampleServiceClient* client,
                               QWidget* parent = nullptr);

  void setToken(const QString& session_token);

  // Attach the live-update feed. When set, each scope change (re)subscribes to
  // WatchSampleList for that scope, and incoming changes trigger a debounced
  // in-place reload of the table. Borrowed; not owned. Pass nullptr to detach.
  void setWatchSubscriber(SampleWatchSubscriber* watch);

  // Scope to a lab (and optionally a single box) selected in the tree.
  void setScope(const QString& lab_id,
                const std::optional<QString>& box_id = std::nullopt);

 private slots:
  // Export the whole current lab to a CSV file (ExportSamplesCsv is lab-scoped).
  void exportCsv();

  // Open the CSV import wizard (dry-run report + all-or-nothing commit) and
  // refresh the list on a committed import.
  void importCsv();

 private:
  SampleServiceClient* client_;
  SampleTableModel* model_ = nullptr;
  SampleSearchBar* search_bar_ = nullptr;
  QTableView* table_ = nullptr;
  SampleWatchSubscriber* watch_ = nullptr;  // borrowed; live-update feed
  QTimer* refresh_timer_ = nullptr;         // debounces a burst of changes
  QString token_;
  QString lab_id_;
  std::optional<QString> box_id_;  // current box scope, for post-import refresh
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_SAMPLEBROWSERWIDGET_H
