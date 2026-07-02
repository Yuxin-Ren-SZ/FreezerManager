// SPDX-License-Identifier: AGPL-3.0-or-later

// gtest entry point for Qt widget tests: a single QApplication must exist before
// any QWidget is constructed. Forces the offscreen platform so the suite runs
// headlessly (no DISPLAY) in CI.

#include <QApplication>
#include <gtest/gtest.h>

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
