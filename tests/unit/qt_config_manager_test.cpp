// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/ConfigManager.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>
#include <QTemporaryDir>

namespace {

  using fmgr::qt::ConfigManager;

  TEST(ConfigManagerTest, DefaultsWhenStoreEmpty) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ConfigManager cfg(dir.filePath("settings.ini"));

    EXPECT_EQ(cfg.serverUrl(), QStringLiteral("0.0.0.0:50051"));
    EXPECT_FALSE(cfg.autoConnect());
    EXPECT_TRUE(cfg.windowGeometry().isEmpty());
  }

  TEST(ConfigManagerTest, ServerUrlRoundTrip) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ConfigManager cfg(dir.filePath("settings.ini"));

    cfg.setServerUrl(QStringLiteral("lab.example.org:50052"));
    EXPECT_EQ(cfg.serverUrl(), QStringLiteral("lab.example.org:50052"));
  }

  TEST(ConfigManagerTest, AutoConnectRoundTrip) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ConfigManager cfg(dir.filePath("settings.ini"));

    cfg.setAutoConnect(true);
    EXPECT_TRUE(cfg.autoConnect());
  }

  TEST(ConfigManagerTest, WindowGeometryRoundTrip) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ConfigManager cfg(dir.filePath("settings.ini"));

    const QByteArray geom = QByteArrayLiteral("\x01\x02\x03\x04");
    cfg.setWindowGeometry(geom);
    EXPECT_EQ(cfg.windowGeometry(), geom);
  }

  TEST(ConfigManagerTest, PersistsAcrossInstances) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("settings.ini");

    {
      ConfigManager cfg(path);
      cfg.setServerUrl(QStringLiteral("persisted:50051"));
      cfg.setAutoConnect(true);
      cfg.sync();
    }

    ConfigManager reopened(path);
    EXPECT_EQ(reopened.serverUrl(), QStringLiteral("persisted:50051"));
    EXPECT_TRUE(reopened.autoConnect());
  }

} // namespace
