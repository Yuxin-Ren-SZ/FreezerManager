// SPDX-License-Identifier: AGPL-3.0-or-later
import { setupServer } from 'msw/node';

/**
 * MSW server for the unit tests (G-arch 2).
 *
 * It starts with no handlers: a test adds exactly the ones it needs with
 * `server.use(...)` and `src/test/setup.ts` resets them afterwards.
 * `onUnhandledRequest: 'error'` (also in setup.ts) makes a request that no
 * handler matches fail the test instead of hitting the network, so a test can
 * never quietly depend on a real server.
 *
 * G1.2 adds the shared `fakeApi({ fail: { '<rpc>': 'PERMISSION_DENIED' } })`
 * handler factory here, per G-arch 10.
 */
export const server = setupServer();
