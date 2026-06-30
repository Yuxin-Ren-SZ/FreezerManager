// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_BOXGRIDMODEL_H
#define FMGR_QT_BOXGRIDMODEL_H

#include <optional>
#include <string>
#include <vector>

#include <QObject>
#include <QString>

#include "qt/BoxServiceClient.h"

namespace fmgr::qt {

class SampleServiceClient;

// Builds the visual layout of one box: its BoxType positions plus the samples
// occupying them, and performs drag-drop placement via MoveSample. All logic
// lives here (a QObject, not a widget) so it is unit-testable headlessly; the
// QGraphicsView is thin glue that renders cells() and reacts to gridChanged().
class BoxGridModel : public QObject {
  Q_OBJECT

 public:
  struct Occupant {
    QString sample_id;
    QString name;
  };

  struct GridCell {
    QString position_label;
    int row = 0;
    int col = 0;
    std::optional<Occupant> occupant;  // empty when the position is free
  };

  struct MoveResult {
    bool ok = false;
    std::string error;  // server message (e.g. size mismatch) when !ok
  };

  BoxGridModel(BoxServiceClient* boxes, SampleServiceClient* samples,
               QObject* parent = nullptr);

  void setToken(const QString& session_token) { token_ = session_token; }

  // Load a box: resolve its BoxType positions and overlay current occupants.
  // Returns false if the box or its type could not be fetched.
  bool setBox(const QString& lab_id, const QString& box_id);

  // Relocate a sample to a position in the current box. On success the grid is
  // reloaded; on rejection the grid is left unchanged and the error is returned.
  MoveResult moveSample(const QString& sample_id, const QString& dest_position);

  const std::vector<GridCell>& cells() const { return cells_; }
  int rows() const { return rows_; }
  int cols() const { return cols_; }

  // Identity of the currently-loaded box, for callers (e.g. PDF export) that
  // need to re-query the same scope. Empty until setBox() succeeds.
  const QString& labId() const { return lab_id_; }
  const QString& boxId() const { return box_id_; }
  const QString& token() const { return token_; }

 signals:
  void gridChanged();

 private:
  // Rebuild cells_ from positions_ + a fresh ListSamples for the current box.
  bool rebuildOccupants();

  BoxServiceClient* boxes_;
  SampleServiceClient* samples_;
  QString token_;
  QString lab_id_;
  QString box_id_;
  std::vector<BoxServiceClient::PositionRow> positions_;
  std::vector<GridCell> cells_;
  int rows_ = 0;
  int cols_ = 0;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_BOXGRIDMODEL_H
