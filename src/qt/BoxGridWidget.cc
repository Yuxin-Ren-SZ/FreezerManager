// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/BoxGridWidget.h"

#include <QBrush>
#include <QContextMenuEvent>
#include <QFile>
#include <QFileDialog>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPen>
#include <QTimer>

#include "qt/BoxGridModel.h"
#include "qt/BoxMapPdf.h"
#include "qt/LabelPdf.h"

namespace fmgr::qt {
namespace {

constexpr qreal kCell = 64.0;   // cell side, scene units
constexpr qreal kPad = 6.0;     // gap between cells
constexpr int kRoleLabel = 0;   // item data: position label
constexpr int kRoleSample = 1;  // item data: occupying sample id

}  // namespace

BoxGridWidget::BoxGridWidget(BoxGridModel* model, BoxMapPdf* box_map,
                             LabelPdf* labels, QWidget* parent)
    : QGraphicsView(parent), model_(model), box_map_(box_map), labels_(labels) {
  scene_ = new QGraphicsScene(this);
  setScene(scene_);

  toast_ = new QLabel(this);
  toast_->setStyleSheet(QStringLiteral(
      "background: #b00020; color: white; padding: 6px; border-radius: 4px;"));
  toast_->setVisible(false);

  connect(model_, &BoxGridModel::gridChanged, this,
          &BoxGridWidget::rebuildScene);
  rebuildScene();
}

void BoxGridWidget::rebuildScene() {
  scene_->clear();
  for (const auto& cell : model_->cells()) {
    const qreal x = cell.col * (kCell + kPad);
    const qreal y = cell.row * (kCell + kPad);
    auto* rect = scene_->addRect(x, y, kCell, kCell);
    rect->setPen(QPen(::Qt::darkGray));
    const bool occupied = cell.occupant.has_value();
    rect->setBrush(
        QBrush(occupied ? QColor(0x9b, 0xc9, 0xff) : QColor(0xf0, 0xf0, 0xf0)));
    rect->setData(kRoleLabel, cell.position_label);
    rect->setData(kRoleSample, occupied ? cell.occupant->sample_id : QString());

    auto* label = new QGraphicsSimpleTextItem(cell.position_label, rect);
    label->setPos(x + 4, y + 4);
    if (occupied) {
      auto* name = new QGraphicsSimpleTextItem(cell.occupant->name, rect);
      name->setPos(x + 4, y + 22);
    }
  }
}

void BoxGridWidget::cellAt(const QPoint& viewport_pos, QString* label,
                           QString* sample_id) const {
  label->clear();
  sample_id->clear();
  QGraphicsItem* item = itemAt(viewport_pos);
  if (item == nullptr) {
    return;
  }
  // Text items are children of the rect; walk up to the rect that carries data.
  while (item != nullptr && item->data(kRoleLabel).toString().isEmpty()) {
    item = item->parentItem();
  }
  if (item == nullptr) {
    return;
  }
  *label = item->data(kRoleLabel).toString();
  *sample_id = item->data(kRoleSample).toString();
}

void BoxGridWidget::mousePressEvent(QMouseEvent* event) {
  QString label;
  QString sample_id;
  cellAt(event->pos(), &label, &sample_id);
  // Only occupied cells can start a placement drag.
  if (!sample_id.isEmpty()) {
    drag_label_ = label;
    drag_sample_id_ = sample_id;
  } else {
    drag_label_.clear();
    drag_sample_id_.clear();
  }
  QGraphicsView::mousePressEvent(event);
}

void BoxGridWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (!drag_sample_id_.isEmpty()) {
    QString target_label;
    QString target_sample_id;
    cellAt(event->pos(), &target_label, &target_sample_id);
    if (!target_label.isEmpty() && target_label != drag_label_) {
      const auto result = model_->moveSample(drag_sample_id_, target_label);
      if (!result.ok) {
        showToast(QStringLiteral("Cannot place here: %1")
                      .arg(QString::fromStdString(result.error)));
      }
    }
  }
  drag_label_.clear();
  drag_sample_id_.clear();
  QGraphicsView::mouseReleaseEvent(event);
}

void BoxGridWidget::showToast(const QString& message) {
  toast_->setText(message);
  toast_->adjustSize();
  toast_->move(8, height() - toast_->height() - 8);
  toast_->setVisible(true);
  toast_->raise();
  QTimer::singleShot(3000, toast_, [this]() { toast_->setVisible(false); });
}

void BoxGridWidget::contextMenuEvent(QContextMenuEvent* event) {
  QMenu menu(this);
  QAction* map_action = nullptr;
  QAction* labels_action = nullptr;

  if (box_map_ != nullptr) {
    map_action = menu.addAction(QStringLiteral("Print Box &Map…"));
  }
  if (labels_ != nullptr) {
    labels_action = menu.addAction(QStringLiteral("Print Sample &Labels…"));
  }
  if (menu.isEmpty()) {
    return;
  }

  QAction* chosen = menu.exec(event->globalPos());
  if (chosen == map_action) {
    printBoxMap();
  } else if (chosen == labels_action) {
    printLabels();
  }
}

void BoxGridWidget::printBoxMap() {
  if (box_map_ == nullptr || model_->boxId().isEmpty()) {
    return;
  }
  const QByteArray pdf =
      box_map_->generate(model_->labId(), model_->boxId(), model_->token());
  savePdf(pdf, model_->boxId());
}

void BoxGridWidget::printLabels() {
  if (labels_ == nullptr) {
    return;
  }
  std::vector<QString> ids;
  for (const auto& cell : model_->cells()) {
    if (cell.occupant.has_value()) {
      ids.push_back(cell.occupant->sample_id);
    }
  }
  if (ids.empty()) {
    showToast(QStringLiteral("No samples in this box."));
    return;
  }
  const QByteArray pdf = labels_->generate(ids, model_->token());
  savePdf(pdf, QStringLiteral("labels-%1").arg(model_->boxId()));
}

void BoxGridWidget::savePdf(const QByteArray& bytes,
                            const QString& suggested_name) {
  const QString path = QFileDialog::getSaveFileName(
      this, QStringLiteral("Save PDF"), suggested_name + QStringLiteral(".pdf"),
      QStringLiteral("PDF (*.pdf)"));
  if (path.isEmpty()) {
    return;
  }
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    QMessageBox::warning(this, QStringLiteral("Error"),
                         QStringLiteral("Cannot write to %1").arg(path));
    return;
  }
  file.write(bytes);
  file.close();
  showToast(QStringLiteral("PDF saved."));
}

}  // namespace fmgr::qt
