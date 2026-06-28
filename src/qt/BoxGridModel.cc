// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/BoxGridModel.h"

#include <QHash>

#include "qt/SampleServiceClient.h"

namespace fmgr::qt {

BoxGridModel::BoxGridModel(BoxServiceClient* boxes,
                           SampleServiceClient* samples, QObject* parent)
    : QObject(parent), boxes_(boxes), samples_(samples) {}

bool BoxGridModel::setBox(const QString& lab_id, const QString& box_id) {
  lab_id_ = lab_id;
  box_id_ = box_id;
  positions_.clear();
  cells_.clear();
  rows_ = 0;
  cols_ = 0;

  const auto box = boxes_->getBox(token_, box_id);
  if (!box.ok) {
    emit gridChanged();
    return false;
  }

  // Resolve the box's type (no GetBoxType RPC; match in ListBoxTypes).
  const auto types = boxes_->listBoxTypes(token_, lab_id);
  if (!types.ok) {
    emit gridChanged();
    return false;
  }
  bool found = false;
  for (const auto& type : types.box_types) {
    if (type.id == box.box.box_type_id) {
      positions_ = type.positions;
      found = true;
      break;
    }
  }
  if (!found) {
    emit gridChanged();
    return false;
  }

  const bool ok = rebuildOccupants();
  emit gridChanged();
  return ok;
}

bool BoxGridModel::rebuildOccupants() {
  cells_.clear();
  rows_ = 0;
  cols_ = 0;

  SampleFilter filter;
  filter.lab_id = lab_id_;
  filter.box_id = box_id_;
  const auto samples = samples_->listSamples(token_, filter);

  QHash<QString, Occupant> by_position;
  if (samples.ok) {
    for (const auto& sample : samples.samples) {
      if (!sample.position_label.isEmpty()) {
        by_position.insert(sample.position_label, Occupant{sample.id,
                                                           sample.name});
      }
    }
  }

  cells_.reserve(positions_.size());
  for (const auto& pos : positions_) {
    GridCell cell;
    cell.position_label = pos.label;
    cell.row = pos.row;
    cell.col = pos.col;
    auto it = by_position.find(pos.label);
    if (it != by_position.end()) {
      cell.occupant = it.value();
    }
    cells_.push_back(std::move(cell));
    rows_ = std::max(rows_, pos.row + 1);
    cols_ = std::max(cols_, pos.col + 1);
  }
  return samples.ok;
}

BoxGridModel::MoveResult BoxGridModel::moveSample(
    const QString& sample_id, const QString& dest_position) {
  const auto result =
      samples_->moveSample(token_, sample_id, box_id_, dest_position);
  if (!result.ok) {
    return {false, result.error};
  }
  rebuildOccupants();
  emit gridChanged();
  return {true, {}};
}

}  // namespace fmgr::qt
