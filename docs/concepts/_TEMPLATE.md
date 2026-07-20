# <Concept Name>

**Area:** <DFS / Search / Query / Fault tolerance / …>  ·  **Phase:** <n>  ·  **Status:** <stub / drafted / reviewed>

## TL;DR
2–3 sentences. What it is and why we need it. Someone should grasp the gist from this alone.

## The problem it solves
What breaks or is inefficient without it? Motivate it with the naive approach first, then
show why that fails.

## How it works
The actual mechanism. Use diagrams (ASCII is fine), math where it matters, and a small
worked example with real numbers. This is the heart of the note — be concrete, not vague.

## Our implementation in Atlas
- Where it lives: `src/…` (files, key functions).
- Key data structures & why we chose them.
- Design decisions specific to Atlas and the trade-offs behind them.
- A short code sketch of the core routine (pseudocode or real C++).

## Complexity & trade-offs
Time/space complexity of the key operations. What we optimized for and what we gave up.

## Failure modes & edge cases
What goes wrong at boundaries (empty input, node death, hot keys, concurrent writes)? How we
handle or knowingly don't.

## Alternatives we considered
Other approaches and why we didn't pick them (or when we would).

## Interview Q&A
3–5 questions an interviewer might ask, with crisp answers. This makes the note double as
prep.

## References
Papers, docs, source. Prefer primary sources (original papers, official docs).
