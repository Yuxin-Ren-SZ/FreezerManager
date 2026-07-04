#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Live PostgreSQL verification: start a throwaway postgres:16 container, point the
# test suite at it via FMGR_TEST_POSTGRES_URL, run every Postgres-parameterized
# test, and tear the container down. Used to close the review's "PostgreSQL live
# verification" item (doc/review-codebase-2026-07-03.md).
#
# Usage:
#   tools/run-postgres-tests.sh                 # run the Postgres test subset
#   tools/run-postgres-tests.sh -R <regex>      # pass extra args through to ctest
#
# Requires: docker, a configured CMake build (default preset: dev).
set -euo pipefail

PRESET="${FMGR_CMAKE_PRESET:-dev}"
CONTAINER="fmgr-pg-test-$$"
PG_PORT="${FMGR_PG_TEST_PORT:-55432}"
PG_PASSWORD="fmgrtest"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The backup round-trip restores a pg_dump archive, so the server major version
# must be >= the host pg_dump: a newer pg_dump emits GUCs (e.g. transaction_timeout)
# an older server rejects. Default the server image to the host pg_dump major
# version to avoid skew; override with FMGR_PG_IMAGE.
host_pg_major="$(pg_dump --version 2>/dev/null | grep -oE '[0-9]+' | head -1 || true)"
PG_IMAGE="${FMGR_PG_IMAGE:-postgres:${host_pg_major:-16}}"

cleanup() {
  docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo ">> starting $PG_IMAGE as $CONTAINER on port $PG_PORT"
docker run -d --name "$CONTAINER" \
  -e POSTGRES_PASSWORD="$PG_PASSWORD" \
  -e POSTGRES_DB=fmgr_test \
  -p "127.0.0.1:${PG_PORT}:5432" \
  "$PG_IMAGE" >/dev/null

echo ">> waiting for postgres to accept connections"
for i in $(seq 1 60); do
  if docker exec "$CONTAINER" pg_isready -U postgres -d fmgr_test >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 1
done
if [[ "${ready:-0}" != "1" ]]; then
  echo "!! postgres did not become ready" >&2
  docker logs "$CONTAINER" >&2 || true
  exit 1
fi

export FMGR_TEST_POSTGRES_URL="postgresql://postgres:${PG_PASSWORD}@127.0.0.1:${PG_PORT}/fmgr_test"
echo ">> FMGR_TEST_POSTGRES_URL=$FMGR_TEST_POSTGRES_URL"

# Default: only tests that actually touch Postgres (name contains Postgres) plus
# the backend-parameterized conformance/repository suites. Override with args.
if [[ $# -gt 0 ]]; then
  ctest --preset "$PRESET" --output-on-failure "$@"
else
  ctest --preset "$PRESET" --output-on-failure -j"$(nproc)" \
    -R 'Postgres|Backends/'
fi
