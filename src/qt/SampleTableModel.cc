// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/SampleTableModel.h"

namespace fmgr::qt {

SampleTableModel::SampleTableModel(SampleServiceClient* client, QObject* parent)
    : QAbstractTableModel(parent), client_(client) {}

QString SampleTableModel::statusToString(v1::SampleStatus status) {
  switch (status) {
    case v1::SAMPLE_STATUS_ACTIVE:
      return QStringLiteral("Active");
    case v1::SAMPLE_STATUS_CHECKED_OUT:
      return QStringLiteral("Checked out");
    case v1::SAMPLE_STATUS_DEPLETED:
      return QStringLiteral("Depleted");
    case v1::SAMPLE_STATUS_DESTROYED:
      return QStringLiteral("Destroyed");
    case v1::SAMPLE_STATUS_TOMBSTONED:
      return QStringLiteral("Deleted");
    default:
      return QStringLiteral("Unspecified");
  }
}

void SampleTableModel::setScope(const SampleFilter& filter) {
  filter_ = filter;
  reload();
}

void SampleTableModel::reload() {
  beginResetModel();
  rows_.clear();
  next_page_token_.clear();
  has_more_ = false;

  const auto result = client_->listSamples(token_, filter_, QString());
  if (result.ok) {
    rows_ = result.samples;
    next_page_token_ = result.next_page_token;
    has_more_ = !next_page_token_.isEmpty();
  }
  endResetModel();
}

void SampleTableModel::fetchNextPage() {
  const auto result = client_->listSamples(token_, filter_, next_page_token_);
  if (!result.ok) {
    has_more_ = false;
    return;
  }
  if (!result.samples.empty()) {
    const int first = static_cast<int>(rows_.size());
    const int last = first + static_cast<int>(result.samples.size()) - 1;
    beginInsertRows(QModelIndex(), first, last);
    rows_.insert(rows_.end(), result.samples.begin(), result.samples.end());
    endInsertRows();
  }
  next_page_token_ = result.next_page_token;
  has_more_ = !next_page_token_.isEmpty();
}

int SampleTableModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) {
    return 0;
  }
  return static_cast<int>(rows_.size());
}

int SampleTableModel::columnCount(const QModelIndex& parent) const {
  if (parent.isValid()) {
    return 0;
  }
  return kColumnCount;
}

QVariant SampleTableModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || role != ::Qt::DisplayRole) {
    return {};
  }
  if (index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
    return {};
  }
  const SampleServiceClient::SampleRow& row = rows_[index.row()];
  switch (index.column()) {
    case kName:
      return row.name;
    case kBarcode:
      return row.barcode;
    case kStatus:
      return statusToString(row.status);
    case kBox:
      return row.box_id;
    case kPosition:
      return row.position_label;
    case kVolume:
      if (!row.volume_value.has_value()) {
        return {};
      }
      return QStringLiteral("%1 %2")
          .arg(*row.volume_value)
          .arg(row.volume_unit)
          .trimmed();
    case kItemType:
      return row.item_type_id;
    default:
      return {};
  }
}

QVariant SampleTableModel::headerData(int section,
                                      ::Qt::Orientation orientation,
                                      int role) const {
  if (role != ::Qt::DisplayRole || orientation != ::Qt::Horizontal) {
    return {};
  }
  switch (section) {
    case kName:
      return QStringLiteral("Name");
    case kBarcode:
      return QStringLiteral("Barcode");
    case kStatus:
      return QStringLiteral("Status");
    case kBox:
      return QStringLiteral("Box");
    case kPosition:
      return QStringLiteral("Position");
    case kVolume:
      return QStringLiteral("Volume");
    case kItemType:
      return QStringLiteral("Item Type");
    default:
      return {};
  }
}

bool SampleTableModel::canFetchMore(const QModelIndex& parent) const {
  if (parent.isValid()) {
    return false;
  }
  return has_more_;
}

void SampleTableModel::fetchMore(const QModelIndex& parent) {
  if (parent.isValid() || !has_more_) {
    return;
  }
  fetchNextPage();
}

}  // namespace fmgr::qt
