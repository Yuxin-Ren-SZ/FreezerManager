// SPDX-License-Identifier: AGPL-3.0-or-later
import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

// G-arch 12 (dev loop): listen on 127.0.0.1 only, on the slot's port from
// scripts/agent/env.sh (5173 + 10 * AGENT_SLOT), and proxy /api to the slot's
// freezerd REST listener. `changeOrigin: false` keeps the original Host
// header, so the gateway's G0.1 Origin check sees matching hosts.
const restListen = process.env.FMGR_REST_LISTEN ?? '127.0.0.1:18080';
const devPort = Number.parseInt(process.env.FMGR_WEB_DEV_PORT ?? '5173', 10);

export default defineConfig({
  plugins: [react()],
  server: {
    host: '127.0.0.1',
    port: devPort,
    // Fail loudly instead of silently moving to another port: an agent that
    // thinks it is talking to its own freezerd must not be on someone else's.
    strictPort: true,
    proxy: {
      '/api': {
        target: `http://${restListen}`,
        changeOrigin: false,
      },
    },
  },
  test: {
    // G-arch 2: Vitest + React Testing Library + MSW. `src/test/setup.ts`
    // registers the matchers, the MSW lifecycle and RTL cleanup, so tests stay
    // free of global setup (globals: false).
    environment: 'jsdom',
    globals: false,
    setupFiles: ['src/test/setup.ts'],
    include: ['src/**/*.test.{ts,tsx}'],
    restoreMocks: true,
  },
});
