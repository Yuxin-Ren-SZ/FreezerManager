// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStackedWidget>
#include <QStatusBar>
#include <QString>

namespace fmgr::qt {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("FreezerManager"));

  pages_ = new QStackedWidget(this);
  // Placeholder landing page; later modules push real pages (login, browser,
  // sample table, box grid) onto this stack.
  auto* placeholder = new QLabel(
      QStringLiteral("FreezerManager desktop client\nConnect to begin."),
      pages_);
  placeholder->setAlignment(::Qt::AlignCenter);
  pages_->addWidget(placeholder);
  setCentralWidget(pages_);

  status_label_ = new QLabel(this);
  statusBar()->addPermanentWidget(status_label_);

  buildMenus();

  const QByteArray geometry = config_.windowGeometry();
  if (!geometry.isEmpty()) {
    restoreGeometry(geometry);
  } else {
    resize(1024, 720);
  }

  updateStatus();

  if (config_.autoConnect()) {
    onConnect();
  }
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenus() {
  QMenu* file_menu = menuBar()->addMenu(QStringLiteral("&File"));

  QAction* connect_action = file_menu->addAction(QStringLiteral("&Connect"));
  connect(connect_action, &QAction::triggered, this, &MainWindow::onConnect);

  file_menu->addSeparator();

  QAction* quit_action = file_menu->addAction(QStringLiteral("&Quit"));
  quit_action->setShortcut(QKeySequence::Quit);
  connect(quit_action, &QAction::triggered, this, &MainWindow::onQuit);
}

void MainWindow::onConnect() {
  channel_ = GrpcChannel(config_.serverUrl().toStdString());
  channel_.connect();
  updateStatus();
}

void MainWindow::onQuit() { close(); }

void MainWindow::updateStatus() {
  if (channel_.isConnected()) {
    status_label_->setText(QStringLiteral("Connected: %1")
                               .arg(QString::fromStdString(channel_.target())));
  } else {
    status_label_->setText(QStringLiteral("Not connected"));
  }
}

void MainWindow::closeEvent(QCloseEvent* event) {
  config_.setWindowGeometry(saveGeometry());
  config_.sync();
  event->accept();
}

}  // namespace fmgr::qt
