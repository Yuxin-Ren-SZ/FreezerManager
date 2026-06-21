// SPDX-License-Identifier: AGPL-3.0-or-later

#include <QApplication>

#include "qt/MainWindow.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setOrganizationName(QStringLiteral("FreezerManager"));
  QApplication::setApplicationName(QStringLiteral("freezermanager-qt"));

  fmgr::qt::MainWindow window;
  window.show();

  return QApplication::exec();
}
