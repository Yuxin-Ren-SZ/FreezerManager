// SPDX-License-Identifier: AGPL-3.0-or-later
import { HttpResponse, http } from 'msw';
import { describe, expect, it } from 'vitest';
import { server } from './server';

// These two tests are the proof that the MSW wiring from `setup.ts` is live:
// a registered handler intercepts, and an unregistered request errors instead
// of escaping to the network.

describe('the MSW test server', () => {
  it('intercepts a request once a test registers a handler for it', async () => {
    let handled = false;
    const url = new URL('/api/v1/healthz', globalThis.location.origin);
    server.use(
      http.get(url.href, () => {
        handled = true;
        return HttpResponse.json({ status: 'ok' });
      }),
    );

    const response = await fetch(url);

    expect(response.status).toBe(200);
    expect(handled).toBe(true);
  });

  it('does not carry a handler over from an earlier test', async () => {
    const url = new URL('/api/v1/healthz', globalThis.location.origin);

    await expect(fetch(url)).rejects.toThrow(/unhandled/i);
  });
});
