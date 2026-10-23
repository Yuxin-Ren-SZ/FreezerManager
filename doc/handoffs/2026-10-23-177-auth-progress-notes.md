# Handoff — #177 auth progress and coordination notes

The lead recorded the delivered #181 fixes and the deliberate generic login
feedback for account-locked `PERMISSION_DENIED` responses. TODO retains G2.1's
completed first cut and names the remaining SSE/sign-out (#182) and durable MFA
(#179) work. The deployment order explicitly requires #148 part 1 and the newer
auth gates; the visible session deadline remains #148 part 2.

AGENTS.md now requires short worker and lead STATUS/HANDOFF comments linked to
detailed handoff notes. The CI guide explains that arm64 Darwin ASan supplies no
LeakSanitizer evidence and Linux leak/race checks remain necessary. The board
records active assignments separately; no worker scope or lock changed here.

Validation: `git diff --check` passed. This is documentation only; no tests were
run. Applicable PR CI must pass before merge. Implemented by lead (Codex).
