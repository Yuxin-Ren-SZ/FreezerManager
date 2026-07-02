// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/LabelPdf.h"

#include <QBuffer>
#include <QFont>
#include <QMarginsF>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QRectF>

#include "qt/SampleServiceClient.h"

namespace fmgr::qt {
namespace {

constexpr int kPointDpi = 72;
constexpr qreal kMargin = 36.0;  // 0.5"
constexpr int kCols = 2;         // Avery 2×4 grid
constexpr int kRows = 4;
constexpr int kPerPage = kCols * kRows;

}  // namespace

LabelPdf::LabelPdf(SampleServiceClient* samples) : samples_(samples) {}

LabelPdf::Model LabelPdf::buildModel(const std::vector<QString>& sample_ids,
                                     const QString& session_token) {
  Model m;
  m.labels.reserve(sample_ids.size());
  for (const auto& id : sample_ids) {
    const auto result = samples_->getSample(session_token, id);
    if (!result.ok) {
      continue;  // skip unreadable samples rather than abort the sheet
    }
    Label label;
    label.name = result.sample.name;
    label.barcode = result.sample.barcode;
    label.location = QStringLiteral("%1 · %2").arg(
        result.sample.box_id, result.sample.position_label);
    m.labels.push_back(std::move(label));
  }
  return m;
}

QByteArray LabelPdf::generate(const std::vector<QString>& sample_ids,
                              const QString& session_token) {
  const Model m = buildModel(sample_ids, session_token);

  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);

  QPdfWriter writer(&buffer);
  writer.setResolution(kPointDpi);
  writer.setPageSize(QPageSize(QPageSize::Letter));
  writer.setPageMargins(QMarginsF(kMargin, kMargin, kMargin, kMargin));
  writer.setTitle(QStringLiteral("Sample Labels"));

  QPainter painter(&writer);
  const qreal page_w = writer.width();
  const qreal page_h = writer.height();
  const qreal cell_w = page_w / kCols;
  const qreal cell_h = page_h / kRows;

  QFont name_font = painter.font();
  name_font.setPointSize(14);
  name_font.setBold(true);
  QFont small_font = painter.font();
  small_font.setPointSize(9);
  small_font.setBold(false);

  for (std::size_t i = 0; i < m.labels.size(); ++i) {
    if (i > 0 && i % kPerPage == 0) {
      writer.newPage();
    }
    const int slot = static_cast<int>(i % kPerPage);
    const int row = slot / kCols;
    const int col = slot % kCols;
    const QRectF cell(col * cell_w, row * cell_h, cell_w, cell_h);
    const QRectF inner = cell.adjusted(8, 8, -8, -8);

    painter.setPen(::Qt::black);
    painter.setFont(name_font);
    painter.drawText(QRectF(inner.x(), inner.y(), inner.width(), 24),
                     ::Qt::AlignTop | ::Qt::AlignLeft | ::Qt::TextWordWrap,
                     m.labels[i].name);

    painter.setFont(small_font);
    painter.drawText(QRectF(inner.x(), inner.y() + 30, inner.width(), 16),
                     ::Qt::AlignLeft, m.labels[i].barcode);
    painter.drawText(QRectF(inner.x(), inner.y() + 48, inner.width(), 16),
                     ::Qt::AlignLeft, m.labels[i].location);
  }

  painter.end();
  buffer.close();
  return bytes;
}

}  // namespace fmgr::qt
