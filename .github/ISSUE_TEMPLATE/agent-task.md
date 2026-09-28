---
name: Agent task
about: One claimable slice of work for the multi-agent board (see AGENTS.md → Coordination)
title: "[<TODO-ID>] <short summary>"
labels: ["status:ready"]
---

**TODO ID:** <!-- e.g. F7, C-10, H2.3; "none" for ad-hoc work -->

## Goal

<!-- One paragraph: what this slice delivers and why. Link the PRD section. -->

## Acceptance criteria

- [ ] <!-- observable behaviour, each one testable -->
- [ ] Tests written first; `ctest --preset dev` green
- [ ] Handoff note added under `doc/handoffs/`

## Scope

- **Likely files / modules:** <!-- e.g. src/rest/, tests/integration/rest_gateway_integration_test.cpp -->
- **Out of scope:** <!-- what not to touch; follow-ups go to new issues -->
- **Locks needed:** <!-- none | lock:migration (version N reserved by lead) | lock:proto | lock:deps | lock:ci -->
- **Depends on:** <!-- #issue numbers that must merge first, or "none" -->

## Test plan

<!-- The exact commands the worker will run, including sanitizer presets if
     the change touches memory, concurrency, storage, or parsing code. -->

<!-- Lead: when assigning, add `agent:worker-N` + `status:in-progress`, remove
     `status:ready`, and post an ASSIGN comment with branch, worktree path,
     slot, and any reserved migration number. -->
