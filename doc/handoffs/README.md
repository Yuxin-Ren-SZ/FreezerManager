# Handoff notes

One file per merged slice of work, written by the agent (or person) who did it,
so the next one can pick up without rereading the diff. This is history: status
lives in GitHub Issues, and the roadmap lives in `TODO.md`.

Notes dated before 2026-09-27 were moved here verbatim from `TODO.md`.

## Rules

- **Add a note in the same PR as the work**, never in a later one. The file is
  new, so it never conflicts with anyone else's PR.
- Filename: `YYYY-MM-DD-<issue>-<slug>.md`, dated the day the PR is opened, for
  example `2026-10-02-42-sse-sample-watch.md`.
- Never edit someone else's note. If a later slice changes what it says, say so
  in the new note.
- No PHI. Not in examples, logs or fixtures.

## Template

```markdown
# Handoff note — YYYY-MM-DD, <TODO IDs> <short title> (#<issue>, <agent>)

One paragraph: what this slice delivers and why, with PRD section if relevant.

**Changed:** the files and modules touched, and the key types/functions added.

**Decisions:** anything a future agent could reasonably have done differently,
and why this way. Link the issue comment where it was agreed, if any.

**Tests:** new/changed test files, and the exact commands run with their
results (for example `ctest --preset dev` → 1399/1399).

**Known limitations / follow-ups:** what is deliberately left out, with the
TODO ID or issue number that owns it.
```
