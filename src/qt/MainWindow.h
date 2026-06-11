// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_MAINWINDOW_H
#define FMGR_QT_MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QSplitter>
#include <QStackedWidget>
#include <QToolBar>

class QAction;

namespace fmgr::qt {

namespace pages {
class DashboardPage;
class SampleBrowserPage;
}
namespace widgets { class BoxGridView; }

enum class PageIndex {
  Dashboard = 0,
  Samples,
  BoxGrid,
  Freezer3D,
  PageCount
};

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override = default;

private slots:
  void showDashboard();
  void showSamplesPage();
  void showBoxGridPage();
  void showFreezer3DPage();

private:
  void setupMenuBar();
  void setupSidebar(QWidget* parent);
  void setupStatusBar();
  void setupCentralWidget();
  void setActiveNav(int index);
  void showAboutDialog();

  QSplitter* splitter_ = nullptr;
  QStackedWidget* centralStack_ = nullptr;
  QLabel* connectionLabel_ = nullptr;

  // Sidebar action buttons
  QAction* dashboardAction_ = nullptr;
  QAction* samplesAction_ = nullptr;
  QAction* boxAction_ = nullptr;
  QAction* freezer3DAction_ = nullptr;

  // Pages (owned by centralStack_)
  pages::DashboardPage* dashboardPage_ = nullptr;
  pages::SampleBrowserPage* sampleBrowserPage_ = nullptr;
  widgets::BoxGridView* boxGridPage_ = nullptr;
  QWidget* freezer3DPage_ = nullptr;
};

}  // namespace fmgr::qt

#endif
