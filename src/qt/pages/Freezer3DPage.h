// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_PAGES_FREEZER3DPAGE_H
#define FMGR_QT_PAGES_FREEZER3DPAGE_H

#include "core/freezer.h"
#include "mock/MockData.h"

#include <QQmlContext>
#include <QQuickWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <QLabel>

namespace fmgr::qt::pages {

using namespace fmgr::core;
using namespace fmgr::qt::mock;

/// Freezer 3D visualization page using Qt Quick + Quick 3D.
/// Wraps a QQuickWidget that loads the QML scene.
class Freezer3DPage : public QWidget {
  Q_OBJECT

public:
  explicit Freezer3DPage(QWidget* parent = nullptr)
      : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    quickWidget_ = new QQuickWidget();
    quickWidget_->setResizeMode(QQuickWidget::SizeRootObjectToView);

    // Expose mock data to QML via context properties
    auto freezers = makeFreezers();
    QVariantList freezerList;
    for (const auto& f : freezers) {
      QVariantMap fm;
      fm["name"] = QString::fromStdString(f.name);
      fm["location"] = QString::fromStdString(f.location);
      fm["model"] = QString::fromStdString(f.model);
      fm["temp"] = f.temp_target_c.has_value() ? f.temp_target_c.value() : 0.0;
      freezerList.append(fm);
    }
    quickWidget_->rootContext()->setContextProperty("freezerData", freezerList);

    // Load the QML file
    quickWidget_->setSource(QUrl("qrc:/qml/Freezer3DView.qml"));

    if (quickWidget_->status() == QQuickWidget::Error) {
      // Fallback: show a simple placeholder
      auto* fallback = new QLabel(
          tr("<h2>Freezer 3D View</h2>"
             "<p>Unable to load QML scene.</p>"
             "<p>Ensure the QML resource is compiled into the binary.</p>"));
      fallback->setAlignment(Qt::AlignCenter);
      layout->addWidget(fallback);
    } else {
      layout->addWidget(quickWidget_);
    }
  }

private:
  QQuickWidget* quickWidget_ = nullptr;
};

}  // namespace fmgr::qt::pages

#endif  // FMGR_QT_PAGES_FREEZER3DPAGE_H
