// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MainWindow.h"

#include "mock/MockData.h"
#include "pages/DashboardPage.h"
#include "pages/FreezerExplorerPage.h"
#include "pages/Freezer3DPage.h"
#include "pages/SampleBrowserPage.h"
#include "pages/SampleDetailPage.h"
#include "widgets/BoxGridView.h"

#include <QAction>
#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyleFactory>
#include <QVBoxLayout>
#include <QWidget>

namespace fmgr::qt {

static const char* kStyleSheet = R"(
  QMainWindow,QWidget{background:#0d1117;color:#c9d1d9;font-family:"Segoe UI","Ubuntu",sans-serif;font-size:13px;}
  QMenuBar{background:#161b22;color:#c9d1d9;border-bottom:1px solid #30363d;padding:2px 8px;}
  QMenuBar::item:selected{background:#21262d;border-radius:4px;}
  QMenu{background:#161b22;border:1px solid #30363d;border-radius:6px;padding:4px;}
  QMenu::item{padding:6px 32px 6px 16px;border-radius:4px;}
  QMenu::item:selected{background:#1f6feb;}
  QMenu::separator{height:1px;background:#30363d;margin:4px 8px;}
  QStatusBar{background:#161b22;color:#8b949e;border-top:1px solid #30363d;padding:2px 12px;font-size:12px;}
  QSplitter::handle{background:#30363d;width:1px;}
  QTableView{background:#0d1117;alternate-background-color:#161b22;color:#c9d1d9;gridline-color:#21262d;border:1px solid #30363d;border-radius:8px;selection-background-color:#1f6feb;selection-color:#fff;}
  QTableView::item{padding:6px 12px;border-bottom:1px solid #21262d;}
  QTableView::item:hover{background:#1a2332;}
  QHeaderView::section{background:#161b22;color:#8b949e;border:none;border-bottom:2px solid #30363d;padding:8px 12px;font-weight:600;font-size:12px;}
  QScrollBar:vertical{background:#0d1117;width:8px;border-radius:4px;}
  QScrollBar::handle:vertical{background:#30363d;border-radius:4px;min-height:30px;}
  QScrollBar::handle:vertical:hover{background:#484f58;}
  QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}
  QLineEdit{background:#0d1117;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;padding:7px 12px;selection-background-color:#1f6feb;}
  QLineEdit:focus{border-color:#1f6feb;background:#161b22;}
  QComboBox{background:#0d1117;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;padding:6px 12px;min-width:120px;}
  QComboBox:hover{border-color:#58a6ff;}
  QComboBox QAbstractItemView{background:#161b22;border:1px solid #30363d;border-radius:6px;selection-background-color:#1f6feb;}
  QTextEdit{background:#0d1117;color:#c9d1d9;border:1px solid #30363d;border-radius:8px;padding:12px;}
  QGraphicsView{background:#0d1117;border:1px solid #30363d;border-radius:8px;}
  QScrollArea{background:transparent;border:none;}
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

void MainWindow::setupMenuBar() {
  QMenu* fileMenu = menuBar()->addMenu("&File");
  QAction* quit = fileMenu->addAction("&Quit");
  quit->setShortcut(QKeySequence::Quit);
  connect(quit, &QAction::triggered, qApp, &QApplication::quit);

  QMenu* viewMenu = menuBar()->addMenu("&View");
  auto addV = [&](const QString& t, QKeySequence k, auto s) {
    auto* a = viewMenu->addAction(t); a->setShortcut(k);
    connect(a, &QAction::triggered, this, s);
  };
  addV("&Dashboard", Qt::CTRL|Qt::Key_1, &MainWindow::showDashboard);
  addV("&Samples",   Qt::CTRL|Qt::Key_2, &MainWindow::showSamplesPage);
  addV("&Freezers",  Qt::CTRL|Qt::Key_3, &MainWindow::showFreezersPage);
  addV("&Box Grid",  Qt::CTRL|Qt::Key_4, &MainWindow::showBoxGridPage);
  addV("Freezer &3D",Qt::CTRL|Qt::Key_5, &MainWindow::showFreezer3DPage);
  viewMenu->addSeparator();
  QAction* fs = viewMenu->addAction("Toggle &Full Screen");
  fs->setShortcut(Qt::Key_F11);
  connect(fs, &QAction::triggered, this, [this](){
    isFullScreen() ? showNormal() : showFullScreen(); });

  QMenu* helpMenu = menuBar()->addMenu("&Help");
  connect(helpMenu->addAction("&About"), &QAction::triggered, this, &MainWindow::showAboutDialog);
}

void MainWindow::setupStatusBar() {
  connectionLabel_ = new QLabel("  Offline — Mock Data Mode");
  connectionLabel_->setStyleSheet("color:#d2991d; font-weight:500;");
  statusBar()->addPermanentWidget(connectionLabel_);
}

void MainWindow::setupSidebar(QWidget* sidebarParent) {
  auto* sidebar = new QVBoxLayout(sidebarParent);
  sidebar->setContentsMargins(0,0,0,0); sidebar->setSpacing(0);

  auto* brand = new QLabel("FM");
  brand->setAlignment(Qt::AlignCenter);
  brand->setStyleSheet("font-size:18px;font-weight:800;color:#58a6ff;background:transparent;padding:16px 8px 6px;");
  sidebar->addWidget(brand);

  auto* sep = new QFrame(); sep->setFrameShape(QFrame::HLine);
  sep->setStyleSheet("background:#30363d;max-height:1px;margin:6px 12px;");
  sidebar->addWidget(sep); sidebar->addSpacing(6);

  auto makeBtn = [&](const QString& s) -> QAction* {
    auto* b = new QAction(s, sidebarParent); b->setCheckable(true);
    sidebarParent->addAction(b); return b;
  };

  dashboardAction_ = makeBtn("\xF0\x9F\x8F\xA0  Home");
  samplesAction_   = makeBtn("\xF0\x9F\x94\x8D  Samples");
  freezersAction_  = makeBtn("\xE2\x9D\x84\xEF\xB8\x8F  Freezers");
  boxAction_       = makeBtn("\xF0\x9F\x93\xA6  Boxes");
  freezer3DAction_ = makeBtn("\xF0\x9F\xA7\x8A  3D View");

  connect(dashboardAction_,&QAction::triggered,this,&MainWindow::showDashboard);
  connect(samplesAction_,  &QAction::triggered,this,&MainWindow::showSamplesPage);
  connect(freezersAction_, &QAction::triggered,this,&MainWindow::showFreezersPage);
  connect(boxAction_,      &QAction::triggered,this,&MainWindow::showBoxGridPage);
  connect(freezer3DAction_,&QAction::triggered,this,&MainWindow::showFreezer3DPage);

  sidebar->addStretch();

  sidebarParent->setStyleSheet(R"(
    QWidget#sidebar{background:#161b22;border-right:1px solid #30363d;}
    QWidget#sidebar QAction{color:#8b949e;background:transparent;border:none;border-radius:8px;padding:10px 14px;margin:1px 8px;font-size:13px;font-weight:500;text-align:left;border-left:3px solid transparent;}
    QWidget#sidebar QAction:hover{background:#21262d;color:#c9d1d9;}
    QWidget#sidebar QAction:checked{background:#1a2332;color:#58a6ff;border-left:3px solid #1f6feb;}
  )");
  sidebarParent->setObjectName("sidebar");
}

void MainWindow::setupCentralWidget() {
  auto* central = new QWidget();
  auto* hl = new QHBoxLayout(central);
  hl->setContentsMargins(0,0,0,0); hl->setSpacing(0);

  auto* sidebarW = new QWidget(); sidebarW->setFixedWidth(180);
  setupSidebar(sidebarW);
  hl->addWidget(sidebarW);

  centralStack_ = new QStackedWidget();

  dashboardPage_ = new pages::DashboardPage();
  centralStack_->addWidget(dashboardPage_);

  sampleBrowserPage_ = new pages::SampleBrowserPage();
  centralStack_->addWidget(sampleBrowserPage_);

  freezerExplorerPage_ = new pages::FreezerExplorerPage();
  centralStack_->addWidget(freezerExplorerPage_);

  {
    boxGridPage_ = new widgets::BoxGridView();
    auto boxTypes = mock::makeBoxTypes();
    auto samples = mock::makeSamples();
    std::vector<core::Sample> filtered;
    for (auto& s : samples)
      if (s.box_id.has_value() && s.box_id->to_string().find("f1000000-0000-4000-d000-000000000000") == 0)
        filtered.push_back(s);
    boxGridPage_->loadBox(boxTypes[0], filtered, "Box-1 (9x9 Cryobox)");
    centralStack_->addWidget(boxGridPage_);
  }

  freezer3DPage_ = new pages::Freezer3DPage();
  centralStack_->addWidget(freezer3DPage_);

  sampleDetailPage_ = new pages::SampleDetailPage();
  centralStack_->addWidget(sampleDetailPage_);

  hl->addWidget(centralStack_, 1);
  setCentralWidget(central);
  centralStack_->setCurrentIndex(0);
  setActiveNav(0);
}

void MainWindow::setActiveNav(int idx) {
  dashboardAction_->setChecked(idx==0);
  samplesAction_->setChecked(idx==1);
  freezersAction_->setChecked(idx==2);
  boxAction_->setChecked(idx==3);
  freezer3DAction_->setChecked(idx==4);
}

void MainWindow::showDashboard()    { centralStack_->setCurrentIndex(0); setActiveNav(0); }
void MainWindow::showSamplesPage()  { centralStack_->setCurrentIndex(1); setActiveNav(1); }
void MainWindow::showFreezersPage() { centralStack_->setCurrentIndex(2); setActiveNav(2); }
void MainWindow::showBoxGridPage()  { centralStack_->setCurrentIndex(3); setActiveNav(3); }
void MainWindow::showFreezer3DPage(){ centralStack_->setCurrentIndex(4); setActiveNav(4); }
void MainWindow::showSampleDetailPage() { centralStack_->setCurrentIndex(5); }
void MainWindow::showSampleDetail(const core::Sample& s) {
  sampleDetailPage_->showSample(s);
  showSampleDetailPage();
}

void MainWindow::showAboutDialog() {
  QMessageBox::about(this, "About",
    "<h3 style='color:#58a6ff'>FreezerManager 0.1.0</h3>"
    "<p>Self-hosted freezer & biospecimen management.</p>");
}

}  // namespace fmgr::qt
