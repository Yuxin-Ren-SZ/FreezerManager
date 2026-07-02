// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_BOXMAPPDF_H
#define FMGR_QT_BOXMAPPDF_H

#include <string>
#include <vector>

#include <QByteArray>
#include <QDate>
#include <QPageSize>
#include <QString>

namespace fmgr::qt {

class BoxServiceClient;
class SampleServiceClient;

// Renders a printable box map: the box's position grid with each occupied
// position carrying its sample name. Pure data → PDF, no widget dependency, so
// it is headless-testable. Drawing goes through QPdfWriter/QPainter; the layout
// the renderer paints is exposed via buildModel() so tests can assert on the
// text content without parsing the (flate-compressed) PDF content stream.
class BoxMapPdf {
 public:
  struct Cell {
    QString position_label;
    QString sample_name;  // empty when the position is free
    int row = 0;
    int col = 0;
    bool occupied = false;
  };

  struct Model {
    bool ok = false;
    std::string error;
    QString title;     // box label
    QString subtitle;  // "<rows> × <cols> · <ISO date>"
    int rows = 0;
    int cols = 0;
    std::vector<Cell> cells;
  };

  BoxMapPdf(BoxServiceClient* boxes, SampleServiceClient* samples);

  // Page geometry for generate(); defaults to US Letter.
  void setPageSize(const QPageSize& page_size) { page_size_ = page_size; }

  // Resolve the box (label, geometry, occupants) into the paintable layout.
  Model buildModel(const QString& box_id, const QString& lab_id,
                   const QString& session_token,
                   const QDate& date = QDate::currentDate());

  // Resolve + paint the box map to a self-contained PDF byte stream.
  QByteArray generate(const QString& box_id, const QString& lab_id,
                      const QString& session_token,
                      const QDate& date = QDate::currentDate());

 private:
  BoxServiceClient* boxes_;
  SampleServiceClient* samples_;
  QPageSize page_size_{QPageSize::Letter};
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_BOXMAPPDF_H
