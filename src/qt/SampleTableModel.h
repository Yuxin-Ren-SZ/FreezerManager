// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_SAMPLETABLEMODEL_H
#define FMGR_QT_SAMPLETABLEMODEL_H

#include <vector>

#include <QAbstractTableModel>
#include <QString>

#include "fmgr/v1/sample.grpc.pb.h"
#include "qt/SampleServiceClient.h"

namespace fmgr::qt {

// Table model backing the sample browser. Holds the accumulated rows for the
// current scope and pages more in on demand via the standard QAbstractItemModel
// virtualization hooks (canFetchMore/fetchMore), so a QTableView over a 100k-row
// box only fetches and renders what is scrolled into view.
//
// QAbstractTableModel is a QObject, not a QWidget, so this is fully unit-testable
// without a QApplication — the logic lives here; the view is thin glue.
class SampleTableModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column {
    kName = 0,
    kBarcode,
    kStatus,
    kBox,
    kPosition,
    kVolume,
    kItemType,
    kColumnCount,
  };

  explicit SampleTableModel(SampleServiceClient* client,
                            QObject* parent = nullptr);

  void setToken(const QString& session_token) { token_ = session_token; }

  // Set the active filter; clears existing rows and loads the first page.
  void setScope(const SampleFilter& filter);

  // Human-readable label for a sample status (used by the Status column).
  static QString statusToString(v1::SampleStatus status);

  // QAbstractTableModel.
  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QVariant headerData(int section, ::Qt::Orientation orientation,
                      int role) const override;
  bool canFetchMore(const QModelIndex& parent) const override;
  void fetchMore(const QModelIndex& parent) override;

 private:
  // Fetch the next page (using next_page_token_) and append its rows.
  void fetchNextPage();

  SampleServiceClient* client_;
  QString token_;
  SampleFilter filter_;
  std::vector<SampleServiceClient::SampleRow> rows_;
  QString next_page_token_;
  bool has_more_ = false;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_SAMPLETABLEMODEL_H
