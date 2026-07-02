// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_SAMPLELOOKUPWIDGET_H
#define FMGR_QT_SAMPLELOOKUPWIDGET_H

#include <memory>

#include <QString>
#include <QWidget>

#include "qt/SampleServiceClient.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QVBoxLayout;

namespace fmgr::qt {

class BoxServiceClient;
class LocationPathResolver;

// Single-handed lookup (PRD §9 #1): an autofocused input that, on Enter, resolves
// a scanned/typed barcode or name to one sample and shows its full location path
// (Freezer → … → Box → Position) plus a sample summary — no tree navigation. The
// input is barcode-first (HID wedge scanners send Enter), falling back to a
// lab-wide name/barcode match. Large fonts and a self-clearing input keep it
// usable single-handed with gloves for rapid back-to-back scanning.
class SampleLookupWidget : public QWidget {
  Q_OBJECT

 public:
  SampleLookupWidget(SampleServiceClient* sample_client,
                     BoxServiceClient* box_client, QWidget* parent = nullptr);
  ~SampleLookupWidget() override;

  void setToken(const QString& session_token) { token_ = session_token; }
  void setScope(const QString& lab_id) { lab_id_ = lab_id; }

 signals:
  void sampleSelected(const QString& sample_id);

 protected:
  void showEvent(QShowEvent* event) override;

 private slots:
  void performLookup();

 private:
  // Resolve the query to the matching samples (barcode-first, then a lab-wide
  // name/barcode match).
  std::vector<SampleServiceClient::SampleRow> findMatches(const QString& query);
  void showFound(const SampleServiceClient::SampleRow& sample);
  void showNotFound(const QString& query);
  void showDisambiguation(
      const std::vector<SampleServiceClient::SampleRow>& matches);
  void clearBreadcrumb();

  SampleServiceClient* sample_;
  BoxServiceClient* box_;
  std::unique_ptr<LocationPathResolver> resolver_;
  QString token_;
  QString lab_id_;

  QLineEdit* input_ = nullptr;
  QStackedWidget* results_ = nullptr;
  QWidget* idle_page_ = nullptr;
  QWidget* found_page_ = nullptr;
  QLabel* not_found_label_ = nullptr;
  QListWidget* match_list_ = nullptr;

  QLabel* name_label_ = nullptr;
  QLabel* barcode_label_ = nullptr;
  QLabel* status_badge_ = nullptr;
  QVBoxLayout* breadcrumb_ = nullptr;

  // Disambiguation rows carry their sample so a click resolves the right one.
  std::vector<SampleServiceClient::SampleRow> pending_matches_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_SAMPLELOOKUPWIDGET_H
