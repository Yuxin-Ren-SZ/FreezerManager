// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/SessionManager.h"

#include <chrono>

#include <QCoreApplication>
#include <QTimer>
#include <gtest/gtest.h>

namespace {

  using fmgr::qt::SessionManager;
  using namespace std::chrono_literals;

  // A movable clock the tests advance manually so expiry is deterministic and
  // needs no running event loop.
  class FakeClock {
  public:
    std::chrono::steady_clock::time_point now() const {
      return now_;
    }
    void advance(std::chrono::seconds d) {
      now_ += d;
    }

  private:
    std::chrono::steady_clock::time_point now_{std::chrono::steady_clock::time_point(1000s)};
  };

  // SessionManager is a QObject and therefore non-copyable/non-movable, so the
  // clock is injected into an existing instance rather than returned by value.
  void useClock(SessionManager* mgr, FakeClock* clock) {
    mgr->setClock([clock] { return clock->now(); });
  }

  TEST(SessionManagerTest, EmptyBeforeLogin) {
    SessionManager mgr;
    EXPECT_FALSE(mgr.hasToken());
    EXPECT_FALSE(mgr.isAuthenticated());
    EXPECT_FALSE(mgr.isExpired());
    EXPECT_EQ(mgr.remaining(), 0s);
  }

  TEST(SessionManagerTest, StartSessionWithoutMfaAuthenticates) {
    FakeClock clock;
    SessionManager mgr;
    useClock(&mgr, &clock);

    mgr.startSession("tok", "sid", "uid", /*mfa_required=*/false, 30min);

    EXPECT_TRUE(mgr.hasToken());
    EXPECT_TRUE(mgr.isAuthenticated());
    EXPECT_EQ(mgr.token(), QStringLiteral("tok"));
    EXPECT_EQ(mgr.sessionId(), QStringLiteral("sid"));
    EXPECT_EQ(mgr.userId(), QStringLiteral("uid"));
    EXPECT_FALSE(mgr.mfaRequired());
  }

  TEST(SessionManagerTest, MfaRequiredBlocksAuthUntilSatisfied) {
    FakeClock clock;
    SessionManager mgr;
    useClock(&mgr, &clock);

    mgr.startSession("tok", "sid", "uid", /*mfa_required=*/true, 30min);
    EXPECT_TRUE(mgr.hasToken());
    EXPECT_TRUE(mgr.mfaRequired());
    EXPECT_FALSE(mgr.isAuthenticated());

    mgr.markMfaSatisfied();
    EXPECT_TRUE(mgr.mfaSatisfied());
    EXPECT_TRUE(mgr.isAuthenticated());
  }

  TEST(SessionManagerTest, ExpiresAfterTtl) {
    FakeClock clock;
    SessionManager mgr;
    useClock(&mgr, &clock);

    mgr.startSession("tok", "sid", "uid", false, 30min);
    EXPECT_FALSE(mgr.isExpired());
    EXPECT_EQ(mgr.remaining(), 30min);

    clock.advance(29min);
    EXPECT_FALSE(mgr.isExpired());
    EXPECT_TRUE(mgr.isAuthenticated());
    EXPECT_EQ(mgr.remaining(), 1min);

    clock.advance(1min);
    EXPECT_TRUE(mgr.isExpired());
    EXPECT_FALSE(mgr.isAuthenticated());
    EXPECT_EQ(mgr.remaining(), 0s);
  }

  TEST(SessionManagerTest, ClearDropsSession) {
    SessionManager mgr;
    mgr.startSession("tok", "sid", "uid", false, 30min);
    ASSERT_TRUE(mgr.hasToken());

    mgr.clear();
    EXPECT_FALSE(mgr.hasToken());
    EXPECT_FALSE(mgr.isAuthenticated());
  }

  TEST(SessionManagerTest, StartSessionEmitsSessionStarted) {
    SessionManager mgr;
    int started = 0;
    QObject::connect(&mgr, &SessionManager::sessionStarted, [&started] { ++started; });
    mgr.startSession("tok", "sid", "uid", false, 30min);
    EXPECT_EQ(started, 1);
  }

  TEST(SessionManagerTest, ClearEmitsSessionEndedOnceWhenTokenHeld) {
    SessionManager mgr;
    int ended = 0;
    QObject::connect(&mgr, &SessionManager::sessionEnded, [&ended] { ++ended; });

    mgr.startSession("tok", "sid", "uid", false, 30min);
    mgr.clear();
    EXPECT_EQ(ended, 1);

    // Idempotent: a second clear with no session emits nothing.
    mgr.clear();
    EXPECT_EQ(ended, 1);
  }

  // Auto-logout: the single-shot QTimer must actually fire handleTimeout(), which
  // tears the session down like an explicit logout. Unlike the other tests this
  // drives a real event loop (no injected clock) so the timer path is exercised.
  // A zero TTL clamps the timer to 0ms, firing on the next loop iteration.
  TEST(SessionManagerTest, TimerFiresAutoLogout) {
    int argc = 0;
    char* argv[] = {nullptr};
    QCoreApplication app(argc, argv);

    SessionManager mgr;
    int ended = 0;
    QObject::connect(&mgr, &SessionManager::sessionEnded, [&ended] { ++ended; });

    mgr.startSession("tok", "sid", "uid", /*mfa_required=*/false, std::chrono::seconds(0));
    ASSERT_TRUE(mgr.hasToken());

    // Bound the loop so a broken timer fails as a timeout rather than a hang.
    QTimer::singleShot(200, &app, &QCoreApplication::quit);
    app.exec();

    EXPECT_EQ(ended, 1);
    EXPECT_FALSE(mgr.hasToken());
    EXPECT_FALSE(mgr.isAuthenticated());
    EXPECT_EQ(mgr.token(), QString());
  }

  TEST(SessionManagerTest, MarkMfaSatisfiedEmitsOnce) {
    SessionManager mgr;
    int changed = 0;
    QObject::connect(&mgr, &SessionManager::mfaSatisfiedChanged, [&changed] { ++changed; });
    mgr.startSession("tok", "sid", "uid", true, 30min);

    mgr.markMfaSatisfied();
    mgr.markMfaSatisfied(); // no-op the second time
    EXPECT_EQ(changed, 1);
  }

} // namespace
