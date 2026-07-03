// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_TEST_SUPPORT_FASTAUTH_H
#define FMGR_TEST_SUPPORT_FASTAUTH_H

#include "auth/LocalAuthProvider.h"

namespace fmgr::test {

  [[nodiscard]] inline auth::LocalAuthProviderConfig fast_auth_config() {
    auth::LocalAuthProviderConfig cfg;
    cfg.pwhash_memlimit = 8192;
    cfg.pwhash_opslimit = 1;
    return cfg;
  }

} // namespace fmgr::test

#endif // FMGR_TEST_SUPPORT_FASTAUTH_H
