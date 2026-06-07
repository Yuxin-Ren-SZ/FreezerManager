#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="${REPO_ROOT}/out/logs"
RUN_ALL=false

# Preset → build_type mapping
declare -A BUILD_TYPES=(
    [dev]=Debug
    [release]=Release
    [asan]=Debug
    [ubsan]=Debug
    [tsan]=Debug
    [release-deterministic]=Release
    [coverage]=Debug
)
ALL_PRESETS=(dev release asan ubsan tsan release-deterministic coverage)

usage() {
    echo "Usage: $0 [--all] [--preset PRESET]"
    echo "  --all            Run all presets sequentially"
    echo "  --preset PRESET  Run a specific preset (default: dev)"
    echo ""
    echo "Env vars:"
    echo "  FMGR_TEST_POSTGRES_URL  PostgreSQL connection URL for Postgres tests"
    exit 1
}

PRESET="${PRESET:-dev}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --all)   RUN_ALL=true; shift ;;
        --preset) PRESET="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "Unknown arg: $1"; usage ;;
    esac
done

if "${RUN_ALL}"; then
    PRESETS_TO_RUN=("${ALL_PRESETS[@]}")
else
    PRESETS_TO_RUN=("${PRESET}")
fi

SUMMARY_LOG="${LOG_DIR}/summary-$(date +%Y%m%dT%H%M%S).log"
mkdir -p "${LOG_DIR}"

# Conan derives its generated preset name from the build type, so every Debug
# preset (dev/asan/ubsan/tsan/coverage) emits a preset named "conan-debug". The
# aggregate CMakeUserPresets.json then defines it multiple times and `cmake
# --preset` aborts with `Duplicate preset: "conan-debug"`. Our CMakePresets.json
# references the conan toolchain directly and never uses these, so drop the stale
# aggregate (generation is also disabled per-install below).
rm -f "${REPO_ROOT}/CMakeUserPresets.json"

overall_status=0

run_preset() {
    local preset="$1"
    local build_type="${BUILD_TYPES[$preset]:-Debug}"
    local log_file="${LOG_DIR}/test-${preset}-$(date +%Y%m%dT%H%M%S).log"
    local status=0

    log() { echo "[$(date +%T)] $*" | tee -a "${log_file}"; }

    log "=== FreezerManager test run: ${preset} ==="
    log "Build type: ${build_type}"
    log "Log:        ${log_file}"
    log "Postgres:   ${FMGR_TEST_POSTGRES_URL:-(not set — Postgres tests will skip)}"
    echo "" | tee -a "${log_file}"

    cd "${REPO_ROOT}"

    log "--- conan install ---"
    conan install . \
        --lockfile=conan.lock \
        --output-folder="out/conan/${preset}" \
        --build=missing \
        -s build_type="${build_type}" \
        -s compiler.cppstd=gnu20 \
        -c tools.cmake.cmaketoolchain:user_presets="" \
        2>&1 | tee -a "${log_file}" || { log "FAILED: conan install"; return 1; }

    log "--- cmake configure ---"
    cmake --preset "${preset}" 2>&1 | tee -a "${log_file}" || { log "FAILED: cmake configure"; return 1; }

    log "--- cmake build ---"
    cmake --build --preset "${preset}" 2>&1 | tee -a "${log_file}" || { log "FAILED: cmake build"; return 1; }

    log "--- ctest ---"
    ctest --preset "${preset}" --output-on-failure 2>&1 | tee -a "${log_file}" || status=1

    if [[ ${status} -eq 0 ]]; then
        log "=== PASSED: ${preset} ==="
    else
        log "=== FAILED: ${preset} ==="
    fi

    echo "${log_file}"
    return ${status}
}

echo "Presets to run: ${PRESETS_TO_RUN[*]}" | tee "${SUMMARY_LOG}"
echo "" | tee -a "${SUMMARY_LOG}"

for preset in "${PRESETS_TO_RUN[@]}"; do
    echo ">>> Starting: ${preset}" | tee -a "${SUMMARY_LOG}"
    if log_file=$(run_preset "${preset}"); then
        echo "    PASSED  — log: ${log_file}" | tee -a "${SUMMARY_LOG}"
    else
        echo "    FAILED  — log: ${log_file}" | tee -a "${SUMMARY_LOG}"
        overall_status=1
    fi
    echo "" | tee -a "${SUMMARY_LOG}"
done

echo "=== Summary ===" | tee -a "${SUMMARY_LOG}"
echo "Full summary: ${SUMMARY_LOG}"

exit ${overall_status}
