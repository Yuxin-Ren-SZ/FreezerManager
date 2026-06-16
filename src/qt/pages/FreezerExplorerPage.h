// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_PAGES_FREEZEREXPLORERPAGE_H
#define FMGR_QT_PAGES_FREEZEREXPLORERPAGE_H

#include "core/box.h"
#include "core/freezer.h"
#include "core/sample.h"
#include "mock/MockData.h"

#include <QFrame>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>
#include <unordered_map>
#include <vector>

namespace fmgr::qt::pages {

using namespace fmgr::core;
using namespace fmgr::qt::mock;

// ── Simple fill bar ────────────────────────────────────────────────
class FillBar : public QWidget {
public:
  FillBar(double pct, QWidget* p = nullptr) : QWidget(p), pct_(pct) {
    setFixedHeight(14); setMinimumWidth(60);
  }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter pt(this); pt.setRenderHint(QPainter::Antialiasing);
    pt.setPen(Qt::NoPen);
    pt.setBrush(QColor(22, 27, 34));
    pt.drawRoundedRect(rect(), 3, 3);
    int w = int(width() * pct_ / 100.0);
    if (w > 0) {
      pt.setBrush(pct_ > 80 ? QColor(218,54,51) : pct_ > 60 ? QColor(210,153,29) : QColor(46,160,67));
      pt.drawRoundedRect(0, 0, w, height(), 3, 3);
    }
  }
  double pct_;
};

// ── Freezer card ───────────────────────────────────────────────────
class FreezerCard : public QFrame {
public:
  FreezerCard(const Freezer& fz, const std::vector<fmgr::core::Box>& allBoxes,
              const std::vector<BoxType>& allTypes,
              const std::vector<Sample>& allSamples,
              QWidget* parent = nullptr)
      : QFrame(parent) {
    setObjectName("card");
    setStyleSheet(
      "QFrame#card{background:#161b22;border:1px solid #30363d;border-radius:10px;}");

    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(16, 14, 16, 14);
    l->setSpacing(8);

    // Header row
    auto* hdr = new QHBoxLayout();

    // Temp indicator
    auto* tempDot = new QLabel();
    double t = fz.temp_target_c.value_or(0);
    QString tempColor = t < -150 ? "#3498db" : t < 0 ? "#3fb950" : "#d2991d";
    tempDot->setStyleSheet(QString("font-size:14px; color:%1; background:transparent; font-weight:700;").arg(tempColor));
    tempDot->setText(QString::number(t, 'f', 0) + "\u00B0C");
    hdr->addWidget(tempDot);

    auto* name = new QLabel(QString::fromStdString(fz.name));
    name->setStyleSheet("font-size:15px; font-weight:700; color:#e6edf3; background:transparent;");
    hdr->addWidget(name);

    hdr->addStretch();

    auto* loc = new QLabel(QString::fromStdString(fz.location));
    loc->setStyleSheet("font-size:11px; color:#8b949e; background:transparent;");
    hdr->addWidget(loc);
    l->addLayout(hdr);

    auto* modelLbl = new QLabel(QString::fromStdString(fz.model));
    modelLbl->setStyleSheet("font-size:11px; color:#484f58; background:transparent;");
    l->addWidget(modelLbl);

    // Boxes in this freezer (simplified: match by storage container prefix)
    int totalPos = 0, occupiedPos = 0;
    for (auto& b : allBoxes) {
      // Find matching box type
      const BoxType* bt = nullptr;
      for (auto& btRef : allTypes) {
        if (btRef.id == b.box_type_id) { bt = &btRef; break; }
      }
      if (!bt) continue;

      int posCount = (int)bt->positions.size();
      totalPos += posCount;

      // Count samples in this box
      int occ = 0;
      for (auto& s : allSamples) {
        if (s.box_id.has_value() && s.box_id.value() == b.id &&
            (s.status == SampleStatus::Active || s.status == SampleStatus::CheckedOut))
          ++occ;
      }
      occupiedPos += occ;

      // Box row
      auto* brow = new QHBoxLayout();
      auto* blabel = new QLabel(QString::fromStdString(b.label));
      blabel->setStyleSheet("font-size:12px; color:#c9d1d9; background:transparent; min-width:60px;");
      brow->addWidget(blabel);

      double fill = posCount > 0 ? (100.0 * occ / posCount) : 0;
      auto* bar = new FillBar(fill);
      brow->addWidget(bar, 1);

      auto* pctLabel = new QLabel(QString("%1/%2").arg(occ).arg(posCount));
      pctLabel->setStyleSheet("font-size:11px; color:#8b949e; background:transparent; min-width:50px;");
      brow->addWidget(pctLabel);

      l->addLayout(brow);
    }

    // Summary line
    double totalFill = totalPos > 0 ? (100.0 * occupiedPos / totalPos) : 0;
    auto* sum = new QLabel(QString("Total: %1 / %2 positions used  (%3%)")
                           .arg(occupiedPos).arg(totalPos)
                           .arg(QString::number(totalFill, 'f', 0)));
    sum->setStyleSheet("font-size:11px; color:#8b949e; background:transparent;");
    l->addWidget(sum);
  }
};

// ── Freezer Explorer page ──────────────────────────────────────────
class FreezerExplorerPage : public QWidget {
  Q_OBJECT

public:
  explicit FreezerExplorerPage(QWidget* parent = nullptr) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea{background:transparent;border:none;}");

    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(16);

    auto* title = new QLabel("Freezer Explorer");
    title->setStyleSheet("font-size:24px; font-weight:800; color:#e6edf3;");
    layout->addWidget(title);

    auto* sub = new QLabel("Browse all freezers and their box occupancy");
    sub->setStyleSheet("font-size:13px; color:#8b949e; margin-bottom:8px;");
    layout->addWidget(sub);

    auto freezers = makeFreezers();
    auto boxes = makeBoxes();
    auto boxTypes = makeBoxTypes();
    auto samples = makeSamples();

    for (auto& fz : freezers) {
      auto* card = new FreezerCard(fz, boxes, boxTypes, samples);
      layout->addWidget(card);
    }
    layout->addStretch();

    scroll->setWidget(content);
    outer->addWidget(scroll);
  }
};

}  // namespace fmgr::qt::pages

#endif
