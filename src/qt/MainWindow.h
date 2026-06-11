// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_MAINWINDOW_H
#define FMGR_QT_MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QStackedWidget>

class QAction;

namespace fmgr::core { struct Sample; }

namespace fmgr::qt {

namespace pages {
class DashboardPage;
class SampleBrowserPage;
class FreezerExplorerPage;
class SampleDetailPage;
}
namespace widgets { class BoxGridView; }

enum class PageIndex {
  Dashboard = 0,
  Samples,
  Freezers,
  BoxGrid,
  Freezer3D,
  SampleDetail,
  Count
};

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override = default;

  void showSampleDetail(const core::Sample& sample);

private slots:
  void showDashboard();
  void showSamplesPage();
  void showFreezersPage();
  void showBoxGridPage();
  void showFreezer3DPage();
  void showSampleDetailPage();

private:
  void setupMenuBar();
  void setupSidebar(QWidget* parent);
  void setupStatusBar();
  void setupCentralWidget();
  void setActiveNav(int index);
  void showAboutDialog();

  QStackedWidget* centralStack_ = nullptr;
  QLabel* connectionLabel_ = nullptr;

  QAction* dashboardAction_ = nullptr;
  QAction* samplesAction_ = nullptr;
  QAction* freezersAction_ = nullptr;
  QAction* boxAction_ = nullptr;
  QAction* freezer3DAction_ = nullptr;

  pages::DashboardPage* dashboardPage_ = nullptr;
  pages::SampleBrowserPage* sampleBrowserPage_ = nullptr;
  pages::FreezerExplorerPage* freezerExplorerPage_ = nullptr;
  widgets::BoxGridView* boxGridPage_ = nullptr;
  QWidget* freezer3DPage_ = nullptr;
  pages::SampleDetailPage* sampleDetailPage_ = nullptr;
};

}  // namespace fmgr::qt

#endif
