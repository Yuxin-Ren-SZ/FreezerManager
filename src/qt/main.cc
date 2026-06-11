// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MainWindow.h"

#include <QApplication>
#include <QMessageBox>
#include <iostream>

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  app.setApplicationName("FreezerManager");
  app.setApplicationVersion("0.1.0");
  app.setOrganizationName("FreezerManager");

  try {
    fmgr::qt::MainWindow window;
    window.show();
    return app.exec();
  } catch (const std::exception& e) {
    std::cerr << "FATAL: " << e.what() << std::endl;
    QMessageBox::critical(nullptr, "Fatal Error", e.what());
    return 1;
  }
}
