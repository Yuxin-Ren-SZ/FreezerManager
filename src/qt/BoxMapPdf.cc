// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/BoxMapPdf.h"

#include <algorithm>

#include <QBuffer>
#include <QColor>
#include <QFont>
#include <QMarginsF>
#include <QPainter>
#include <QPdfWriter>
#include <QRectF>

#include "qt/BoxGridModel.h"
#include "qt/BoxServiceClient.h"
#include "qt/SampleServiceClient.h"

namespace fmgr::qt {
namespace {

// Render at 72 dpi so one painter unit equals one PostScript point and the
// page is exactly its QPageSize point dimensions (Letter = 612×792).
constexpr int kPointDpi = 72;
constexpr qreal kMargin = 36.0;     // 0.5"
constexpr qreal kTitleH = 28.0;     // title band height
constexpr qreal kSubtitleH = 16.0;  // metadata line height

}  // namespace

BoxMapPdf::BoxMapPdf(BoxServiceClient* boxes, SampleServiceClient* samples)
    : boxes_(boxes), samples_(samples) {}

BoxMapPdf::Model BoxMapPdf::buildModel(const QString& box_id,
                                       const QString& lab_id,
                                       const QString& session_token,
                                       const QDate& date) {
  Model m;

  const auto box = boxes_->getBox(session_token, box_id);
  if (!box.ok) {
    m.error = box.error;
    return m;
  }
  m.title = box.box.label.isEmpty() ? box_id : box.box.label;

  // Reuse BoxGridModel to resolve the box type's geometry and overlay the
  // current occupants — same logic the on-screen grid uses.
  BoxGridModel grid(boxes_, samples_);
  grid.setToken(session_token);
  if (!grid.setBox(lab_id, box_id)) {
    m.error = "failed to resolve box layout";
    return m;
  }

  m.rows = grid.rows();
  m.cols = grid.cols();
  m.cells.reserve(grid.cells().size());
  for (const auto& c : grid.cells()) {
    Cell cell;
    cell.position_label = c.position_label;
    cell.row = c.row;
    cell.col = c.col;
    cell.occupied = c.occupant.has_value();
    if (cell.occupied) {
      cell.sample_name = c.occupant->name;
    }
    m.cells.push_back(std::move(cell));
  }

  m.subtitle = QStringLiteral("%1 × %2 · %3")
                   .arg(m.rows)
                   .arg(m.cols)
                   .arg(date.toString(::Qt::ISODate));
  m.ok = true;
  return m;
}

QByteArray BoxMapPdf::generate(const QString& box_id, const QString& lab_id,
                               const QString& session_token,
                               const QDate& date) {
  const Model m = buildModel(box_id, lab_id, session_token, date);
  if (!m.ok) {
    return QByteArray();
  }

  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);

  QPdfWriter writer(&buffer);
  writer.setResolution(kPointDpi);
  writer.setPageSize(page_size_);
  writer.setPageMargins(QMarginsF(kMargin, kMargin, kMargin, kMargin));
  writer.setTitle(m.title);

  QPainter painter(&writer);
  const qreal page_w = writer.width();
  const qreal page_h = writer.height();

  // Title + metadata band.
  QFont title_font = painter.font();
  title_font.setPointSize(16);
  title_font.setBold(true);
  painter.setFont(title_font);
  painter.drawText(QRectF(0, 0, page_w, kTitleH),
                   ::Qt::AlignLeft | ::Qt::AlignVCenter, m.title);

  QFont meta_font = painter.font();
  meta_font.setPointSize(10);
  meta_font.setBold(false);
  painter.setFont(meta_font);
  painter.drawText(QRectF(0, kTitleH, page_w, kSubtitleH),
                   ::Qt::AlignLeft | ::Qt::AlignVCenter, m.subtitle);

  // Grid area beneath the band.
  const qreal grid_top = kTitleH + kSubtitleH + 8.0;
  const qreal grid_h = page_h - grid_top - kSubtitleH;  // leave a footer line
  if (m.rows > 0 && m.cols > 0 && grid_h > 0) {
    const qreal cell_w = page_w / m.cols;
    const qreal cell_h = grid_h / m.rows;
    QFont label_font = painter.font();
    label_font.setPointSize(7);

    for (const auto& cell : m.cells) {
      const qreal x = cell.col * cell_w;
      const qreal y = grid_top + cell.row * cell_h;
      const QRectF rect(x, y, cell_w, cell_h);
      painter.fillRect(rect, cell.occupied ? QColor(0x9b, 0xc9, 0xff)
                                           : QColor(0xff, 0xff, 0xff));
      painter.setPen(QColor(0x80, 0x80, 0x80));
      painter.drawRect(rect);

      painter.setPen(::Qt::black);
      painter.setFont(label_font);
      painter.drawText(rect.adjusted(3, 2, -3, -2),
                       ::Qt::AlignTop | ::Qt::AlignLeft, cell.position_label);
      if (cell.occupied) {
        painter.drawText(
            rect.adjusted(3, 2, -3, -2),
            ::Qt::AlignBottom | ::Qt::AlignLeft | ::Qt::TextWordWrap,
            cell.sample_name);
      }
    }
  }

  // Footer page number.
  painter.setFont(meta_font);
  painter.setPen(QColor(0x60, 0x60, 0x60));
  painter.drawText(QRectF(0, page_h - kSubtitleH, page_w, kSubtitleH),
                   ::Qt::AlignRight | ::Qt::AlignVCenter,
                   QStringLiteral("Page 1"));

  painter.end();
  buffer.close();
  return bytes;
}

}  // namespace fmgr::qt
