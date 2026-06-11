// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MainWindow.h"

#include "mock/MockData.h"
#include "pages/DashboardPage.h"
#include "pages/SampleBrowserPage.h"
#include "pages/Freezer3DPage.h"
#include "widgets/BoxGridView.h"

#include <QAction>
#include <QApplication>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyleFactory>
#include <QVBoxLayout>
#include <QWidget>

namespace fmgr::qt {

// ── Global dark stylesheet ─────────────────────────────────────────
static const char* kStyleSheet = R"(
  QMainWindow, QWidget {
    background-color: #0d1117; color: #c9d1d9;
    font-family: "Segoe UI","Ubuntu","Helvetica Neue",sans-serif; font-size:13px;
  }
  QMenuBar { background:#161b22; color:#c9d1d9; border-bottom:1px solid #30363d; padding:2px 8px; }
  QMenuBar::item:selected { background:#21262d; border-radius:4px; }
  QMenu { background:#161b22; border:1px solid #30363d; border-radius:6px; padding:4px; }
  QMenu::item { padding:6px 32px 6px 16px; border-radius:4px; }
  QMenu::item:selected { background:#1f6feb; }
  QMenu::separator { height:1px; background:#30363d; margin:4px 8px; }
  QStatusBar { background:#161b22; color:#8b949e; border-top:1px solid #30363d; padding:2px 12px; font-size:12px; }
  QSplitter::handle { background:#30363d; width:1px; }
  QTableView { background:#0d1117; alternate-background-color:#161b22; color:#c9d1d9;
    gridline-color:#21262d; border:1px solid #30363d; border-radius:8px;
    selection-background-color:#1f6feb; selection-color:#fff; }
  QTableView::item { padding:6px 12px; border-bottom:1px solid #21262d; }
  QTableView::item:hover { background:#1a2332; }
  QHeaderView::section { background:#161b22; color:#8b949e; border:none;
    border-bottom:2px solid #30363d; padding:8px 12px; font-weight:600; font-size:12px; }
  QScrollBar:vertical { background:#0d1117; width:8px; border-radius:4px; }
  QScrollBar::handle:vertical { background:#30363d; border-radius:4px; min-height:30px; }
  QScrollBar::handle:vertical:hover { background:#484f58; }
  QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
  QLineEdit { background:#0d1117; color:#c9d1d9; border:1px solid #30363d;
    border-radius:6px; padding:7px 12px; selection-background-color:#1f6feb; }
  QLineEdit:focus { border-color:#1f6feb; background:#161b22; }
  QComboBox { background:#0d1117; color:#c9d1d9; border:1px solid #30363d;
    border-radius:6px; padding:6px 12px; min-width:120px; }
  QComboBox:hover { border-color:#58a6ff; }
  QComboBox QAbstractItemView { background:#161b22; border:1px solid #30363d;
    border-radius:6px; selection-background-color:#1f6feb; }
  QTextEdit { background:#0d1117; color:#c9d1d9; border:1px solid #30363d;
    border-radius:8px; padding:12px; }
  QGraphicsView { background:#0d1117; border:1px solid #30363d; border-radius:8px; }
  QScrollArea { background:transparent; border:none; }
)";

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("FreezerManager");
  resize(1320, 860);
  setMinimumSize(1000, 640);
  qApp->setStyleSheet(kStyleSheet);
  QApplication::setStyle(QStyleFactory::create("Fusion"));

  setupMenuBar();
  setupStatusBar();
  setupCentralWidget();
}

// ── Menu bar ───────────────────────────────────────────────────────
void MainWindow::setupMenuBar() {
  QMenu* fileMenu = menuBar()->addMenu("&File");
  QAction* quitAction = fileMenu->addAction("&Quit");
  quitAction->setShortcut(QKeySequence::Quit);
  connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

  QMenu* viewMenu = menuBar()->addMenu("&View");
  auto addView = [&](const QString& t, QKeySequence k, auto slot) {
    auto* a = viewMenu->addAction(t); a->setShortcut(k);
    connect(a, &QAction::triggered, this, slot);
  };
  addView("&Dashboard", Qt::CTRL | Qt::Key_1, &MainWindow::showDashboard);
  addView("&Samples",   Qt::CTRL | Qt::Key_2, &MainWindow::showSamplesPage);
  addView("&Box Grid",  Qt::CTRL | Qt::Key_3, &MainWindow::showBoxGridPage);
  addView("Freezer &3D",Qt::CTRL | Qt::Key_4, &MainWindow::showFreezer3DPage);
  viewMenu->addSeparator();
  QAction* fs = viewMenu->addAction("Toggle &Full Screen");
  fs->setShortcut(QKeySequence(Qt::Key_F11));
  connect(fs, &QAction::triggered, this, [this]() {
    if (isFullScreen()) showNormal(); else showFullScreen();
  });

  QMenu* helpMenu = menuBar()->addMenu("&Help");
  QAction* about = helpMenu->addAction("&About FreezerManager");
  connect(about, &QAction::triggered, this, &MainWindow::showAboutDialog);
}

// ── Status bar ─────────────────────────────────────────────────────
void MainWindow::setupStatusBar() {
  connectionLabel_ = new QLabel("  Offline — Mock Data Mode");
  connectionLabel_->setStyleSheet("color:#d2991d; font-weight:500;");
  statusBar()->addPermanentWidget(connectionLabel_);
  statusBar()->showMessage("Ready", 2000);
}

// ── Sidebar ────────────────────────────────────────────────────────
void MainWindow::setupSidebar(QWidget* sidebarParent) {
  auto* sidebar = new QVBoxLayout(sidebarParent);
  sidebar->setContentsMargins(0, 0, 0, 0);
  sidebar->setSpacing(0);

  // App title area
  auto* brand = new QLabel("Freezer\nManager");
  brand->setAlignment(Qt::AlignCenter);
  brand->setStyleSheet(
    "font-size:13px; font-weight:800; color:#58a6ff; background:transparent;"
    "padding:16px 8px 8px 8px;");
  sidebar->addWidget(brand);

  auto* sep1 = new QFrame();
  sep1->setFrameShape(QFrame::HLine);
  sep1->setStyleSheet("background:#30363d; max-height:1px; margin:4px 12px;");
  sidebar->addWidget(sep1);

  sidebar->addSpacing(8);

  // Sidebar buttons — styled via a helper lambda
  auto makeSideBtn = [&](const QString& icon, const QString& label) -> QAction* {
    auto* btn = new QAction(QString("%1  %2").arg(icon, label), sidebarParent);
    btn->setCheckable(true);
    // Rich styling for sidebar items
    sidebarParent->addAction(btn);
    return btn;
  };

  dashboardAction_  = makeSideBtn("\xF0\x9F\x8F\xA0", "Home");
  samplesAction_    = makeSideBtn("\xF0\x9F\x94\x8D", "Samples");
  boxAction_        = makeSideBtn("\xF0\x9F\x93\xA6", "Boxes");
  freezer3DAction_  = makeSideBtn("\xF0\x9F\xA7\x8A", "3D View");

  connect(dashboardAction_, &QAction::triggered, this, &MainWindow::showDashboard);
  connect(samplesAction_,   &QAction::triggered, this, &MainWindow::showSamplesPage);
  connect(boxAction_,       &QAction::triggered, this, &MainWindow::showBoxGridPage);
  connect(freezer3DAction_, &QAction::triggered, this, &MainWindow::showFreezer3DPage);

  sidebar->addStretch();

  // Sidebar style as a stylesheet targeting the sidebar widget
  sidebarParent->setStyleSheet(R"(
    QWidget#sidebar {
      background: #161b22;
      border-right: 1px solid #30363d;
    }
    QWidget#sidebar QAction {
      color: #8b949e;
      background: transparent;
      border: none;
      border-radius: 8px;
      padding: 10px 16px;
      margin: 2px 8px;
      font-size: 13px;
      font-weight: 500;
      text-align: left;
      border-left: 3px solid transparent;
    }
    QWidget#sidebar QAction:hover {
      background: #21262d;
      color: #c9d1d9;
    }
    QWidget#sidebar QAction:checked {
      background: #1a2332;
      color: #58a6ff;
      border-left: 3px solid #1f6feb;
    }
  )");
  sidebarParent->setObjectName("sidebar");
}

// ── Central area ───────────────────────────────────────────────────
void MainWindow::setupCentralWidget() {
  auto* central = new QWidget();
  auto* hlayout = new QHBoxLayout(central);
  hlayout->setContentsMargins(0, 0, 0, 0);
  hlayout->setSpacing(0);

  // ── Sidebar ──
  auto* sidebarWidget = new QWidget();
  sidebarWidget->setFixedWidth(180);
  setupSidebar(sidebarWidget);
  hlayout->addWidget(sidebarWidget);

  // ── Content stack ──
  centralStack_ = new QStackedWidget();

  // Page 0: Dashboard
  dashboardPage_ = new pages::DashboardPage();
  centralStack_->addWidget(dashboardPage_);

  // Page 1: Sample Browser
  sampleBrowserPage_ = new pages::SampleBrowserPage();
  centralStack_->addWidget(sampleBrowserPage_);

  // Page 2: Box Grid
  {
    boxGridPage_ = new widgets::BoxGridView();
    auto boxType = mock::make9x9BoxType();
    auto samples = mock::makeSamples();
    std::vector<core::Sample> box1Samples;
    for (const auto& s : samples) {
      if (s.box_id.has_value() &&
          s.box_id->to_string().find("aaaa0000-bbbb-4000-d000") == 0) {
        box1Samples.push_back(s);
      }
    }
    boxGridPage_->loadBox(boxType, box1Samples, "Box 1 (9x9 Cryobox)");
    centralStack_->addWidget(boxGridPage_);
  }

  // Page 3: Freezer 3D
  freezer3DPage_ = new pages::Freezer3DPage();
  centralStack_->addWidget(freezer3DPage_);

  hlayout->addWidget(centralStack_, 1);

  setCentralWidget(central);

  // Start on Dashboard
  centralStack_->setCurrentIndex(static_cast<int>(PageIndex::Dashboard));
  setActiveNav(0);
}

// ── Page navigation ────────────────────────────────────────────────
void MainWindow::setActiveNav(int idx) {
  dashboardAction_->setChecked(idx == 0);
  samplesAction_->setChecked(idx == 1);
  boxAction_->setChecked(idx == 2);
  freezer3DAction_->setChecked(idx == 3);
}

void MainWindow::showDashboard() {
  centralStack_->setCurrentIndex(static_cast<int>(PageIndex::Dashboard));
  setActiveNav(0);
}
void MainWindow::showSamplesPage() {
  centralStack_->setCurrentIndex(static_cast<int>(PageIndex::Samples));
  setActiveNav(1);
}
void MainWindow::showBoxGridPage() {
  centralStack_->setCurrentIndex(static_cast<int>(PageIndex::BoxGrid));
  setActiveNav(2);
}
void MainWindow::showFreezer3DPage() {
  centralStack_->setCurrentIndex(static_cast<int>(PageIndex::Freezer3D));
  setActiveNav(3);
}

void MainWindow::showAboutDialog() {
  QMessageBox::about(this, "About FreezerManager",
    "<h3 style='color:#58a6ff'>FreezerManager 0.1.0</h3>"
    "<p>Self-hosted freezer & biospecimen management system.</p>"
    "<p>AGPLv3 / Commercial dual license.</p>");
}

}  // namespace fmgr::qt
