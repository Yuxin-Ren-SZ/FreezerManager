// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_WIDGETS_BOXGRIDVIEW_H
#define FMGR_QT_WIDGETS_BOXGRIDVIEW_H

#include "core/box.h"
#include "core/sample.h"

#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsTextItem>
#include <QLabel>
#include <QPainter>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>
#include <unordered_map>

namespace fmgr::qt::widgets {

using namespace fmgr::core;

/// Visual representation of a single position in a box grid.
class PositionItem : public QGraphicsRectItem {
public:
  PositionItem(const Position& pos, QGraphicsItem* parent = nullptr)
      : QGraphicsRectItem(parent), position_(pos) {
    setAcceptHoverEvents(true);
    setToolTip(QString::fromStdString(pos.label));
  }

  const Position& position() const { return position_; }

  void setOccupied(bool occupied, const QString& sampleName = {}) {
    occupied_ = occupied;
    if (occupied) {
      setBrush(QColor(31, 111, 235));   // blue accent
      setPen(QPen(QColor(31, 111, 235), 1));
      sampleName_ = sampleName;
      setToolTip(QString("%1 - %2").arg(QString::fromStdString(position_.label), sampleName));
    } else {
      setBrush(QColor(22, 27, 34));      // dark bg
      setPen(QPen(QColor(48, 54, 61), 1));
      sampleName_.clear();
      setToolTip(QString::fromStdString(position_.label));
    }
  }

  void setCheckedOut() {
    setBrush(QColor(210, 153, 29));  // amber/accent
    setPen(QPen(QColor(210, 153, 29), 1));
  }

  void setDepleted() {
    setBrush(QColor(72, 79, 88));  // subtle gray
    setPen(QPen(QColor(72, 79, 88), 1));
  }

protected:
  void hoverEnterEvent(QGraphicsSceneHoverEvent*) override {
    setPen(QPen(QColor(88, 166, 255), 2));
  }
  void hoverLeaveEvent(QGraphicsSceneHoverEvent*) override {
    setPen(QPen(occupied_ ? QColor(31, 111, 235) : QColor(48, 54, 61), 1));
  }

private:
  Position position_;
  bool occupied_ = false;
  QString sampleName_;
};

/// QGraphicsView-based box grid visualization.
/// Takes a BoxType (defines the grid layout) and a list of Samples
/// currently placed in this box.
class BoxGridView : public QWidget {
  Q_OBJECT

public:
  explicit BoxGridView(QWidget* parent = nullptr)
      : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    scene_ = new QGraphicsScene(this);

    view_ = new QGraphicsView(scene_);
    view_->setRenderHint(QPainter::Antialiasing);
    view_->setDragMode(QGraphicsView::ScrollHandDrag);
    view_->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    view_->setResizeAnchor(QGraphicsView::AnchorUnderMouse);
    view_->setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    view_->setOptimizationFlags(
        QGraphicsView::DontSavePainterState |
        QGraphicsView::DontAdjustForAntialiasing);

    layout->addWidget(view_);

    // Info label below grid
    infoLabel_ = new QLabel(tr("Click a position to see details."));
    infoLabel_->setAlignment(Qt::AlignCenter);
    layout->addWidget(infoLabel_);
  }

  /// Rebuild the grid for a given BoxType and sample placement.
  void loadBox(const BoxType& boxType,
               const std::vector<Sample>& samples,
               const QString& boxLabel = {}) {
    scene_->clear();
    positionItems_.clear();

    if (boxType.positions.empty()) return;

    // Determine grid dimensions
    int maxRow = 0, maxCol = 0;
    for (const auto& p : boxType.positions) {
      maxRow = std::max(maxRow, p.row);
      maxCol = std::max(maxCol, p.col);
    }

    const double cellSize = 34.0;
    const double margin = 20.0;
    const double headerSize = 25.0;

    // Column headers (1, 2, 3, …)
    for (int c = 0; c <= maxCol; ++c) {
      auto* text = scene_->addText(QString::number(c + 1));
      text->setPos(margin + headerSize + c * cellSize + cellSize / 2 - 5,
                   margin);
    }

    // Row headers (A, B, C, …)
    for (int r = 0; r <= maxRow; ++r) {
      auto* text = scene_->addText(QString(static_cast<char>('A' + r)));
      text->setPos(margin, margin + headerSize + r * cellSize + cellSize / 2 - 8);
    }

    // Build map: position_label → sample for quick lookup
    std::unordered_map<std::string, const Sample*> sampleMap;
    for (const auto& s : samples) {
      if (s.position_label.has_value()) {
        sampleMap[s.position_label.value()] = &s;
      }
    }

    // Draw cells
    for (const auto& pos : boxType.positions) {
      double x = margin + headerSize + pos.col * cellSize;
      double y = margin + headerSize + pos.row * cellSize;
      auto* item = new PositionItem(pos);
      item->setRect(x, y, cellSize, cellSize);
      item->setPen(QPen(Qt::gray, 1));

      auto it = sampleMap.find(pos.label);
      if (it != sampleMap.end()) {
        const auto* s = it->second;
        QString name = QString::fromStdString(s->name);
        if (s->status == SampleStatus::Active) {
          item->setOccupied(true, name);
        } else if (s->status == SampleStatus::CheckedOut) {
          item->setOccupied(true, name);
          item->setCheckedOut();
        } else if (s->status == SampleStatus::Depleted) {
          item->setOccupied(true, name);
          item->setDepleted();
        }
      }

      scene_->addItem(item);
      positionItems_[pos.label] = item;
    }

    // Title
    auto* titleText = scene_->addText(
        boxLabel.isEmpty() ? QString::fromStdString(boxType.name) : boxLabel);
    titleText->setPos(margin, margin + headerSize + (maxRow + 1) * cellSize + 10);
    QFont titleFont = titleText->font();
    titleFont.setBold(true);
    titleText->setFont(titleFont);

    // Set scene rect
    double sceneW = margin + headerSize + (maxCol + 1) * cellSize + margin;
    double sceneH = margin + headerSize + (maxRow + 1) * cellSize + margin + 30;
    scene_->setSceneRect(0, 0, sceneW, sceneH);

    view_->fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
  }

  QGraphicsView* view() const { return view_; }
  QGraphicsScene* scene() const { return scene_; }

private:
  QGraphicsScene* scene_ = nullptr;
  QGraphicsView* view_ = nullptr;
  QLabel* infoLabel_ = nullptr;
  std::unordered_map<std::string, PositionItem*> positionItems_;
};

}  // namespace fmgr::qt::widgets

#endif  // FMGR_QT_WIDGETS_BOXGRIDVIEW_H
