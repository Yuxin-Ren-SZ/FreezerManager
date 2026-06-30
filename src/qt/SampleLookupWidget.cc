// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/SampleLookupWidget.h"

#include <QColor>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QShowEvent>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "fmgr/v1/sample.grpc.pb.h"
#include "qt/BoxServiceClient.h"
#include "qt/LocationPathResolver.h"

namespace fmgr::qt {
namespace {

struct StatusStyle {
  QString text;
  QColor color;
};

StatusStyle statusStyle(v1::SampleStatus status) {
  switch (status) {
    case v1::SAMPLE_STATUS_ACTIVE:
      return {QStringLiteral("Active"), QColor(0x0a, 0x7d, 0x33)};
    case v1::SAMPLE_STATUS_CHECKED_OUT:
      return {QStringLiteral("Checked out"), QColor(0xb8, 0x6e, 0x00)};
    case v1::SAMPLE_STATUS_DEPLETED:
      return {QStringLiteral("Depleted"), QColor(0x6c, 0x6c, 0x6c)};
    case v1::SAMPLE_STATUS_DESTROYED:
      return {QStringLiteral("Destroyed"), QColor(0xb0, 0x00, 0x20)};
    case v1::SAMPLE_STATUS_TOMBSTONED:
      return {QStringLiteral("Deleted"), QColor(0xb0, 0x00, 0x20)};
    default:
      return {QStringLiteral("Unspecified"), QColor(0x6c, 0x6c, 0x6c)};
  }
}

QString segmentPrefix(LocationPathResolver::PathSegment::Kind kind) {
  using Kind = LocationPathResolver::PathSegment::Kind;
  switch (kind) {
    case Kind::Freezer:
      return QStringLiteral("Freezer");
    case Kind::Box:
      return QStringLiteral("Box");
    case Kind::Position:
      return QStringLiteral("Position");
    case Kind::Container:
      return QString();  // the container kind name is carried in detail
  }
  return QString();
}

}  // namespace

SampleLookupWidget::SampleLookupWidget(SampleServiceClient* sample_client,
                                       BoxServiceClient* box_client,
                                       QWidget* parent)
    : QWidget(parent),
      sample_(sample_client),
      box_(box_client),
      resolver_(std::make_unique<LocationPathResolver>(box_client)) {
  auto* layout = new QVBoxLayout(this);

  input_ = new QLineEdit(this);
  input_->setObjectName(QStringLiteral("lookupInput"));
  input_->setPlaceholderText(
      QStringLiteral("Scan or type a sample barcode or name, then Enter"));
  input_->setClearButtonEnabled(true);
  QFont input_font = input_->font();
  input_font.setPointSize(input_font.pointSize() + 8);
  input_->setFont(input_font);
  input_->setMinimumHeight(48);
  // setFocus() on the widget (e.g. the Ctrl+L shortcut) lands on the input.
  setFocusProxy(input_);
  layout->addWidget(input_);

  results_ = new QStackedWidget(this);
  layout->addWidget(results_, 1);

  // Page 0: idle.
  idle_page_ = new QLabel(QStringLiteral("Ready to scan."), results_);
  qobject_cast<QLabel*>(idle_page_)->setAlignment(::Qt::AlignCenter);
  results_->addWidget(idle_page_);

  // Page 1: found — a result card with the path breadcrumb and a summary.
  found_page_ = new QWidget(results_);
  auto* card = new QVBoxLayout(found_page_);

  name_label_ = new QLabel(found_page_);
  name_label_->setObjectName(QStringLiteral("sampleName"));
  QFont name_font = name_label_->font();
  name_font.setPointSize(name_font.pointSize() + 10);
  name_font.setBold(true);
  name_label_->setFont(name_font);
  card->addWidget(name_label_);

  auto* summary = new QHBoxLayout;
  barcode_label_ = new QLabel(found_page_);
  barcode_label_->setObjectName(QStringLiteral("sampleBarcode"));
  status_badge_ = new QLabel(found_page_);
  status_badge_->setObjectName(QStringLiteral("statusBadge"));
  summary->addWidget(barcode_label_);
  summary->addStretch(1);
  summary->addWidget(status_badge_);
  card->addLayout(summary);

  auto* rule = new QFrame(found_page_);
  rule->setFrameShape(QFrame::HLine);
  card->addWidget(rule);

  auto* path_view = new QWidget(found_page_);
  path_view->setObjectName(QStringLiteral("pathView"));
  breadcrumb_ = new QVBoxLayout(path_view);
  breadcrumb_->setContentsMargins(0, 0, 0, 0);
  card->addWidget(path_view);
  card->addStretch(1);
  results_->addWidget(found_page_);

  // Page 2: not found.
  not_found_label_ = new QLabel(results_);
  not_found_label_->setObjectName(QStringLiteral("notFoundLabel"));
  not_found_label_->setAlignment(::Qt::AlignCenter);
  QFont nf_font = not_found_label_->font();
  nf_font.setPointSize(nf_font.pointSize() + 4);
  not_found_label_->setFont(nf_font);
  results_->addWidget(not_found_label_);

  // Page 3: disambiguation.
  match_list_ = new QListWidget(results_);
  match_list_->setObjectName(QStringLiteral("matchList"));
  results_->addWidget(match_list_);

  results_->setCurrentWidget(idle_page_);

  connect(input_, &QLineEdit::returnPressed, this,
          &SampleLookupWidget::performLookup);
  connect(match_list_, &QListWidget::itemActivated, this,
          [this](QListWidgetItem* item) {
            const int idx = match_list_->row(item);
            if (idx >= 0 && idx < static_cast<int>(pending_matches_.size())) {
              showFound(pending_matches_[idx]);
            }
          });
}

SampleLookupWidget::~SampleLookupWidget() = default;

void SampleLookupWidget::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  input_->setFocus();
}

std::vector<SampleServiceClient::SampleRow> SampleLookupWidget::findMatches(
    const QString& query) {
  // Barcode-first: the common case is a scanned barcode, which the server can
  // filter directly.
  SampleFilter barcode_filter;
  barcode_filter.lab_id = lab_id_;
  barcode_filter.barcode = query;
  const auto by_barcode = sample_->listSamples(token_, barcode_filter);
  if (by_barcode.ok && !by_barcode.samples.empty()) {
    return by_barcode.samples;
  }

  // Fall back to a lab-wide name/barcode match (ListSamples has no name filter).
  SampleFilter lab_filter;
  lab_filter.lab_id = lab_id_;
  const auto lab_wide = sample_->listSamples(token_, lab_filter);
  std::vector<SampleServiceClient::SampleRow> matches;
  for (const auto& row : lab_wide.samples) {
    if (row.name.compare(query, ::Qt::CaseInsensitive) == 0 ||
        row.barcode.compare(query, ::Qt::CaseInsensitive) == 0) {
      matches.push_back(row);
    }
  }
  return matches;
}

void SampleLookupWidget::performLookup() {
  const QString query = input_->text().trimmed();
  if (query.isEmpty()) {
    return;
  }

  const auto matches = findMatches(query);
  if (matches.size() == 1) {
    showFound(matches.front());
    // Self-clearing input for rapid back-to-back scanning.
    input_->clear();
    input_->setFocus();
    emit sampleSelected(matches.front().id);
  } else if (matches.empty()) {
    showNotFound(query);
  } else {
    showDisambiguation(matches);
  }
}

void SampleLookupWidget::clearBreadcrumb() {
  QLayoutItem* item = nullptr;
  while ((item = breadcrumb_->takeAt(0)) != nullptr) {
    delete item->widget();
    delete item;
  }
}

void SampleLookupWidget::showFound(
    const SampleServiceClient::SampleRow& sample) {
  name_label_->setText(sample.name);
  barcode_label_->setText(
      sample.barcode.isEmpty()
          ? QStringLiteral("(no barcode)")
          : QStringLiteral("Barcode: %1").arg(sample.barcode));
  const StatusStyle style = statusStyle(sample.status);
  status_badge_->setText(style.text);
  status_badge_->setStyleSheet(
      QStringLiteral("color: white; background: %1; padding: 2px 8px; "
                     "border-radius: 4px;")
          .arg(style.color.name()));

  clearBreadcrumb();
  const auto path =
      resolver_->resolve(token_, lab_id_, sample.box_id, sample.position_label);
  if (!path.ok) {
    auto* err = new QLabel(QStringLiteral("Location unavailable: %1")
                               .arg(QString::fromStdString(path.error)),
                           found_page_);
    breadcrumb_->addWidget(err);
  } else if (!path.placed) {
    breadcrumb_->addWidget(
        new QLabel(QStringLiteral("Unplaced — not in any box."), found_page_));
  } else {
    for (const auto& seg : path.segments) {
      const QString prefix =
          seg.detail.isEmpty() ? segmentPrefix(seg.kind) : seg.detail;
      auto* row = new QLabel(
          prefix.isEmpty() ? seg.label
                           : QStringLiteral("%1: %2").arg(prefix, seg.label),
          found_page_);
      QFont seg_font = row->font();
      seg_font.setPointSize(seg_font.pointSize() + 4);
      row->setFont(seg_font);
      breadcrumb_->addWidget(row);
    }
    if (path.partial) {
      breadcrumb_->addWidget(new QLabel(
          QStringLiteral("(partial path — container chain incomplete)"),
          found_page_));
    }
  }

  results_->setCurrentWidget(found_page_);
}

void SampleLookupWidget::showNotFound(const QString& query) {
  not_found_label_->setText(
      QStringLiteral("No sample matches “%1”.").arg(query));
  results_->setCurrentWidget(not_found_label_);
}

void SampleLookupWidget::showDisambiguation(
    const std::vector<SampleServiceClient::SampleRow>& matches) {
  pending_matches_ = matches;
  match_list_->clear();
  for (const auto& row : matches) {
    const QString label =
        row.barcode.isEmpty()
            ? row.name
            : QStringLiteral("%1  ·  %2").arg(row.name, row.barcode);
    match_list_->addItem(label);
  }
  results_->setCurrentWidget(match_list_);
}

}  // namespace fmgr::qt
