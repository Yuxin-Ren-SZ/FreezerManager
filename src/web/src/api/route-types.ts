// SPDX-License-Identifier: AGPL-3.0-or-later
import type { DescMessage } from '@bufbuild/protobuf';

/**
 * The shape of one unary REST route (TODO.md G1.2, G-arch 4/5).
 *
 * The types are deliberately narrow strings — `path` must be under `/api/v1/`
 * and `rpc` must be a canonical `fmgr.v1.<Service>/<Method>` name — because
 * `scripts/check-routes.mjs` cross-checks both against the `FMGR_ROUTE(...)`
 * lines in `src/rest/RestGateway.cc`, and `scripts/gen.mjs` generates the
 * schemas they point at.
 */
export interface UnaryRoute {
  /** The REST path exactly as registered by the gateway. */
  readonly path: `/api/v1/${string}`;
  /** Canonical proto RPC name, `<package>.<Service>/<Method>`. */
  readonly rpc: `fmgr.v1.${string}/${string}`;
  /** Generated schema for the request message. */
  readonly input: DescMessage;
  /** Generated schema for the response message. */
  readonly output: DescMessage;
}
