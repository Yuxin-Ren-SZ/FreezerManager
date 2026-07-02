// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_BOXGRIDWIDGET_H
#define FMGR_QT_BOXGRIDWIDGET_H

#include <QGraphicsView>
#include <QString>

class QGraphicsScene;
class QLabel;

namespace fmgr::qt {

class BoxGridModel;
class BoxMapPdf;
class LabelPdf;

// Thin QGraphicsView that draws a box's positions as a row/col grid of cells and
// lets the user drag an occupied cell onto another cell to place a sample. The
// move is delegated to BoxGridModel; a rejected move (e.g. size mismatch) is
// shown as a transient toast. A context menu exports the box map / labels to
// PDF via BoxMapPdf / LabelPdf (optional — null disables those actions). All
// data/validation lives in the model — this is glue covered by manual e2e, not
// headless unit tests.
class BoxGridWidget : public QGraphicsView {
  Q_OBJECT

 public:
  explicit BoxGridWidget(BoxGridModel* model, BoxMapPdf* box_map = nullptr,
                         LabelPdf* labels = nullptr, QWidget* parent = nullptr);

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void contextMenuEvent(QContextMenuEvent* event) override;

 private:
  void rebuildScene();
  void showToast(const QString& message);
  // Export the current box map to a user-chosen PDF file.
  void printBoxMap();
  // Export labels for the box's occupied samples to a user-chosen PDF file.
  void printLabels();
  // Write bytes to a QFileDialog-chosen path; toasts on failure.
  void savePdf(const QByteArray& bytes, const QString& suggested_name);
  // Returns the position label of the cell under the given viewport point, plus
  // the occupying sample id (empty if none); label empty if no cell there.
  void cellAt(const QPoint& viewport_pos, QString* label,
              QString* sample_id) const;

  BoxGridModel* model_;
  BoxMapPdf* box_map_;
  LabelPdf* labels_;
  QGraphicsScene* scene_;
  QLabel* toast_;
  QString drag_label_;
  QString drag_sample_id_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_BOXGRIDWIDGET_H
