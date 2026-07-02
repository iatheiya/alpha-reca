# CHAIN modifier system — design proposal (NOITO unification)

**Status: design only, not implemented.** Authored by the project owner
through an iterative design conversation (2026-06-20/21); this document
consolidates the final agreed shape, to be implemented next.

## Motivation

`NOITO` and `CHAIN` (`loader.py`, `mode == 'noito'` / `mode == 'chain'`)
are near-duplicate preprocessor constructs. Verified directly against the
code: both share the exact same name-resolution logic (`_parse_iris_mode`,
`_auto_name`) and the exact same sub-item passthrough rule (`rel > 0` →
raw, unchanged). They differ in exactly one respect — how `NOLINK` is
emitted:

- `NOITO`: every body item (`rel == 0`) gets its own `NOLINK` immediately
  before it. No upfront `NOLINK` at the opener.
- `CHAIN`: one `NOLINK` is emitted once, at the opener line. Items within
  the block are *not* individually `NOLINK`'d — they auto-link to each
  other in sequence (hence "chain").

Given how small and mechanical this difference is, and that the project
already has a working precedent for "simple thing = parametrized special
case of the general thing" (`FOR`'s anonymous count blocks, `ALLOC_LUX`
as a fixed-count `ALLOC_TO`), `NOITO` is retired entirely and folded into
`CHAIN` as a modifier.

## Current behavior (for reference — what's being replaced)

Both `NOITO` and `CHAIN` currently support the same three name-resolution
forms via `_parse_iris_mode`:

| Form | Generated item name |
|------|---------------------|
| `CHAIN` (no argument) | `ITO {first token of line} {rest}` — manual: author supplies the name as the first word of each body line |
| `CHAIN NAME` | `ITO NAME_{idx} {op...}` (or `NAME_{suffix}_{idx}` if the line's first token starts with `_`) |
| `CHAIN _` | `ITO __ch_{idx} {line}` — fully anonymous |

Sub-items (any indentation deeper than the body level, `rel > 0`) are
passed through completely unchanged — no `ITO`, no name, no `NOLINK`.
This lets the author write arbitrary manual code at that depth, fully
outside `CHAIN`/`NOITO`'s influence. Verified identical between `NOITO`
and `CHAIN` today; carries over unchanged in the new design.

## New syntax

```
CHAIN [name-form] | [modifier modifier ...]
```

The name-form (`NAME`, `_`, or absent) goes before `|`, exactly as
today. Modifiers go after `|`, space-separated, **order-independent**.
`|` was chosen because it isn't used anywhere in real Reca syntax today
(checked — the only hits are inside comments describing bitwise OR).

### Modifiers identified so far

- **`NOLINK`** — every generated item gets its own `NOLINK` (the old
  `NOITO` behavior). Without this modifier, items auto-link to each
  other as `CHAIN` already does today.
- **`ANON`** — generated items get anonymous naming (`__ch_{idx}`)
  regardless of whether a block name was given. See the `NAME | ANON`
  interaction below — this is *not* redundant with `CHAIN _`.

Each prior `NOITO` call site becomes `CHAIN ... | NOLINK` (plus `ANON`
if the old call also used anonymous naming).

### Why `CHAIN NAME | ANON` is not the same as `CHAIN _`

The block itself can have its own identity, independent of how its
generated items are named. `CHAIN NAME` alone uses `NAME` for *both* the
block's identity *and* the generated item names (`NAME_0`, `NAME1`, ...).
`CHAIN NAME | ANON` keeps `NAME` as the block's own label (for whatever
purpose the author has — readability, documentation, a future use this
design doesn't need to specify) while `ANON` independently overrides
*item* naming to be anonymous regardless. `CHAIN _` (no separate name at
all, anonymous mode signaled the original way) has no block identity —
there's nothing for `ANON` to leave alone, since there was never a name
to begin with. So `NAME | ANON` and `_` are genuinely different: one has
a named block with anonymous items, the other has neither.

### Auto-naming convention: `NAME` is `NAME`'s own zeroth element

Updated from today's `NAME_{idx}` (always includes the index, including
0, with an underscore) to the same convention already established for
`FOR`'s `{Y}`/`{Y1}`/`{Y2}` override placeholders: bare `NAME` *is* the
zeroth item (no suffix at all), and explicit numbering starts at 1
without an underscore (`NAME1`, `NAME2`, ...). This applies because
`CHAIN`'s generated names are purely internal, arbitrary identifiers
with no external convention to clash with — the same category that
justified the `Y`/`Y1` convention, as distinct from e.g. `A0`-`A3`
syscall-argument registers (which carry external ABI meaning and were
explicitly *not* renamed under this convention earlier in the project).

## Per-item link-override marker: `-`

### The gap this closes

With `NOLINK` active, every item is independent by default. But within
that same block, one specific item may need to actually link to whatever
came immediately before it — while still getting the block's normal
auto-generated name (so it has to stay at the `rel == 0` auto-naming
level, not drop to manual sub-indentation, which would lose the
auto-naming sequence entirely and force writing the `ITO <name>` and
linking logic out by hand). This is a real, previously-encountered gap:
several `FOR` conversions earlier in this session needed exactly this
kind of "keep the auto-generated identity, but make this one item behave
differently" capability and `FOR` had no way to provide it without
manually pulling that one line out of the loop.

### Syntax

A `-` token, written as its own separate, space-separated token at the
start of a body line (`- Op`, not `-Op`), means "this item links
backward to whatever came immediately before it" — i.e., suppress the
block's default `NOLINK` for this one line specifically, regardless of
what `NOLINK`/no-`NOLINK` the rest of the block uses.

- `-` was chosen over `~` because `~` already has an established,
  different meaning in the language — an escape character inside packed
  string templates (`~` → LF, used in `parser.re`'s string-packing
  logic). Reusing it here for an unrelated purpose would overload one
  symbol with two unrelated meanings in different contexts. `-` has no
  conflicting usage anywhere in real Reca syntax (checked).
- "Links backward to the previous item" was chosen over "links forward
  to the next item" for local readability — reading line N alone tells
  you everything about line N's behavior, without needing to check the
  next line to know whether it will be retroactively linked. This is a
  readability judgment, not a functional one: either direction is
  technically expressible for any given pair of items, just written on
  the other side of the pair.
- The marker only applies at the `rel == 0` auto-naming level — it has
  no meaning at `rel > 0` (sub-items are raw passthrough regardless, as
  always).

## Architecture: independent modifier dimensions, not branching combinations

This is the part of the design that makes arbitrary modifier
combinations work without writing new code for each combination.
Naming-mode (block-named / anonymous / manual) and linking-mode
(auto-link / per-item NOLINK / per-item override via `-`) are two
**independent dimensions** that don't know about each other:

- The naming step decides *what to call* the generated item — it does
  not know or care whether that item will be `NOLINK`'d.
- The linking step decides *whether to emit `NOLINK`* before the item —
  it does not know or care what the item is named.

Implemented as a flag set parsed once at the `CHAIN` opener
(`{'NOLINK': bool, 'ANON': bool}`), not as a combinatorial tree of
`if/elif` branches per combination. Every combination of currently-known
modifiers therefore works without any additional code written
specifically for that combination, because the two steps never inspect
each other's state.

**Explicit limit, stated honestly so it isn't later treated as a broken
promise:** this "compose for free" property holds only because `NOLINK`
and `ANON` are genuinely independent today. If some future modifier
needs to behave differently *depending on* whether another modifier is
also active — a real interaction, not mere coexistence — that specific
interaction will need its own code. No architecture can avoid this; it's
a property of composition itself (independent pieces compose for free,
dependent ones don't), not a shortcoming of this design. The honest claim
this design makes is narrower and still true: *for any modifiers that
remain independent of each other*, no per-combination code is needed.

## Compatibility / migration

- `NOITO` is retired entirely (per the author's decision). All 5 current
  `NOITO` call sites (1 in `parser.re`, 4 in `yaku.re`) migrate to
  `CHAIN ... | NOLINK`.
- All 12 current `CHAIN` call sites keep working with zero syntax change
  (no `|` present → no modifiers → identical behavior to today), *except*
  for the auto-naming index-0 convention change (`NAME_0` → bare `NAME`),
  which is a real behavior change for any existing `CHAIN NAME` call site
  whose body has 2+ items — needs to be checked against each real call
  site during implementation, the same way every `FOR` conversion this
  session was checked against the frozen binary's actual field values
  before being considered done.
- A Latin replacement name for `CHAIN` itself was discussed (`Series`,
  `Ordo`, `Catena`, `Nexus`, `Vinculum`, `Filum`, and ~15 others) — none
  resonated with the author. Staying with `CHAIN` for now; revisit later
  if a better name comes up. Not a blocker for implementation.

## Open items for implementation (not yet resolved, flag during coding)

1. Exact parsing of the `|`-separated modifier list at the `CHAIN`
   opener — needs the same kind of precise empirical verification this
   session has applied everywhere else (isolated test cases before
   touching real `.re` files, then real conversions checked against the
   frozen binary's actual field values, not just "freeze() didn't
   crash").
2. Whether `CHAIN _ | NOLINK` (explicit anonymous name-slot *combined*
   with modifiers) is intended to be valid — implied by the orthogonal-
   dimensions design (name-slot and modifier-list don't interact), but
   not explicitly exercised in any example discussed so far. Should be
   covered by the implementation's test cases regardless.
