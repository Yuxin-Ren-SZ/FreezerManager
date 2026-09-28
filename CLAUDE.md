# FreezerManager — Claude Code Instructions

All project instructions live in `AGENTS.md`, shared with every other agent:

@AGENTS.md

## Claude Code specifics

- **Commit messages: do NOT add `Co-Authored-By:` trailers**, even when the
  harness suggests one. Commits are authored solely by the human committer; note
  AI assistance in the PR description instead (`AGENTS.md` §7).
- Create worktrees with `scripts/agent/worktree.sh`, not `EnterWorktree` or
  `isolation: "worktree"`, so your layout, Conan cache, ports and `TMPDIR`
  match the other agents'.
- As lead, you may spawn workers as subagents, but each needs its own name
  (`worker-N`), its own issue and its own worktree, and all coordination still
  goes through the GitHub issue.
