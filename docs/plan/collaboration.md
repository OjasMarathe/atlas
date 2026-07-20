# Atlas — How We Work Together

Two people, both with Claude Code, aiming for a stellar shared artifact. Model: **hybrid —
co-design the contracts together, then build subsystems in parallel behind those contracts,
reviewing each other via PRs.**

## Principles

1. **Contracts before code.** We agree on the `proto/` gRPC interfaces *together* before
   either of us implements behind them. Once fixed, we can't step on each other.
2. **`main` is always green and always demoable.** No direct pushes to `main`. Everything
   lands via reviewed PR. CI must pass.
3. **A feature isn't done until its concept note is written.** Every PR that implements a
   concept updates the matching file in [../concepts/](../concepts/). This is the whole point.
4. **Small PRs > big PRs.** Easier to review, faster to merge, cleaner history.
5. **The repo should read like real infrastructure** — because on our resumes, it is.

## Ownership split (suggested — agree together, swap freely)

The clean seam is **storage-side vs search-side**, meeting at the gRPC contract.

| Track | Owner | Phases |
|---|---|---|
| Storage spine — DFS, replication, fault tolerance | **Ojas** | 1, 2 |
| Search spine — index, ranking, query engine | **Harshal** | 3, 4 |
| Co-designed together | both | 0 (contracts, cluster model), and the hard cores: ring, Raft |

Owning a track ≠ working alone — you still review every PR on the other track, and we pair
on the genuinely hard shared pieces (consistent-hashing ring, replication protocol, Raft).
For post-M1 phases we re-split when we get there.

## Git workflow

- **Branch off `main`** for every unit of work. Naming: `<track>/<short-desc>`, e.g.
  `dfs/chunking`, `dfs/consistent-hashing`, `search/inverted-index`, `infra/ci`.
- **Conventional Commits:** `feat:`, `fix:`, `docs:`, `test:`, `refactor:`, `chore:`,
  `perf:`. Example: `feat(dfs): add virtual-node consistent hashing ring`.
- **Rebase on `main`** before opening a PR to keep history linear.
- **Open a PR** early (draft is fine) so the other can see direction.
- **Merge:** squash-merge into `main` once approved + CI green. Delete the branch.

## Pull-request checklist

Every PR description covers:

- **What & why** — one paragraph.
- **How tested** — commands run, what you observed (or new tests added).
- **Concepts documented** — link the concept note(s) added/updated. *(A code PR with no
  concept-note change should be rare and justified.)*
- **Checklist:**
  - [ ] Builds locally + CI green
  - [ ] Sanitizers clean (ASan/TSan) for touched concurrency code
  - [ ] Concept note added/updated
  - [ ] `progress.md` updated if a phase milestone moved
  - [ ] No secrets / large binaries committed

## Reviewing each other's code

- Turn PRs around reasonably quickly — a stalled PR blocks the other person.
- Review for: correctness, the concurrency model (races, deadlocks, lock scope),
  clear naming, and **whether the concept note actually explains the code**.
- Use GitHub review comments; approve explicitly. Disagreements → discuss on the PR or
  a quick call, decide, record the outcome (an ADR if it's architectural).

## Using Claude Code well (we both have it)

- Point Claude at the relevant `proto/` file + concept note so it builds to the contract.
- Ask it to draft the concept note *from* the code you just wrote, then edit for accuracy —
  fast way to keep docs in lock-step with implementation.
- Have the *reviewer's* Claude do an adversarial pass on the author's PR (find the race,
  the unhandled failure) before human review.
- Keep prompts/decisions that worked in the ADRs or concept notes so the other benefits.

## Cadence

- **Contracts sync** at the start of each phase (align on the interface before building).
- **Async by default**, short sync when a PR needs live discussion.
- Keep [progress.md](progress.md) current so either of us can resume the other's thread cold.

## Definition of "phase complete"

All DoD boxes in [roadmap.md](roadmap.md) checked, all owed concept notes written and
reviewed, merged to `main`, CI green, and the phase's slice demoable.
