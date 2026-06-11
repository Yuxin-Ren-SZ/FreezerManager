// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_MODELS_SAMPLETABLEMODEL_H
#define FMGR_QT_MODELS_SAMPLETABLEMODEL_H

#include "core/sample.h"

#include <QAbstractTableModel>
#include <QColor>
#include <QString>
#include <vector>

namespace fmgr::qt::models {

using namespace fmgr::core;

/// QAbstractTableModel wrapping a vector of Sample for display in QTableView.
/// Columns: Name | Barcode | Box | Position | Status | Volume | Type
class SampleTableModel : public QAbstractTableModel {
  Q_OBJECT

public:
  enum Column {
    ColName = 0,
    ColBarcode,
    ColBox,
    ColPosition,
    ColStatus,
    ColVolume,
    ColType,
    ColumnCount  // sentinel
  };

  explicit SampleTableModel(QObject* parent = nullptr)
      : QAbstractTableModel(parent) {}

  void setSamples(std::vector<Sample> samples) {
    beginResetModel();
    samples_ = std::move(samples);
    endResetModel();
  }

  const std::vector<Sample>& samples() const { return samples_; }

  // --- QAbstractTableModel interface ---
  int rowCount(const QModelIndex& parent = {}) const override {
    if (parent.isValid()) return 0;
    return static_cast<int>(samples_.size());
  }

  int columnCount(const QModelIndex& parent = {}) const override {
    if (parent.isValid()) return 0;
    return ColumnCount;
  }

  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
    if (!index.isValid()) return {};
    const auto& s = samples_.at(index.row());

    if (role == Qt::DisplayRole) {
      switch (index.column()) {
        case ColName:     return QString::fromStdString(s.name);
        case ColBarcode:  return QString::fromStdString(s.barcode.value_or("—"));
        case ColBox:      return QString::fromStdString(s.box_id.has_value()
                               ? s.box_id->to_string().substr(0, 8) + "…" : "—");
        case ColPosition: return QString::fromStdString(s.position_label.value_or("—"));
        case ColStatus:   return statusString(s.status);
        case ColVolume:   return s.volume_value.has_value()
                               ? QString("%1 µL").arg(s.volume_value.value()) : "—";
        case ColType:     return QString::fromStdString(s.item_type_id.to_string().substr(0, 8) + "…");
      }
    }

    if (role == Qt::ForegroundRole) {
      if (s.status == SampleStatus::CheckedOut) return QColor("#e67e22"); // orange
      if (s.status == SampleStatus::Depleted)   return QColor("#95a5a6"); // gray
      if (s.status == SampleStatus::Destroyed)  return QColor("#c0392b"); // red
    }

    if (role == Qt::ToolTipRole && index.column() == ColStatus) {
      return statusString(s.status);
    }

    return {};
  }

  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
      case ColName:     return tr("Name");
      case ColBarcode:  return tr("Barcode");
      case ColBox:      return tr("Box");
      case ColPosition: return tr("Position");
      case ColStatus:   return tr("Status");
      case ColVolume:   return tr("Volume");
      case ColType:     return tr("Type");
    }
    return {};
  }

private:
  static QString statusString(SampleStatus s) {
    switch (s) {
      case SampleStatus::Active:     return "Active";
      case SampleStatus::CheckedOut: return "Checked Out";
      case SampleStatus::Depleted:   return "Depleted";
      case SampleStatus::Destroyed:  return "Destroyed";
      case SampleStatus::Tombstoned: return "Tombstoned";
    }
    return "?";
  }

  std::vector<Sample> samples_;
};

}  // namespace fmgr::qt::models

#endif  // FMGR_QT_MODELS_SAMPLETABLEMODEL_H
