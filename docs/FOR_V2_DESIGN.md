# FOR macro v2 — design proposal

**Status: implemented and verified.** Authored by the project owner
through an iterative design conversation (2026-06-20); this document
consolidates the final agreed shape. Implementation is in `loader.py`'s
`_expand_indent` (helper functions `_for_parse_header`,
`_for_parse_selector_token`, `_for_parse_override_line`, `_for_expand`,
plus the `'for'` mode dispatch and `_close_ctx` case).

## Implementation notes

- **Backward compatibility confirmed**: all 7 pre-existing `FOR` usages
  in the project produce byte-identical output (verified by direct
  field-value inspection of the frozen binary, not just "freeze() didn't
  crash").
- **Real bug caught during first real-world conversion**: a `FOR` body
  line with no `ITO <name>` prefix (e.g. bare `Add El1=... Exit=...`)
  is not a recognized command anywhere in the rest of the pipeline (it's
  an operation name, normally the *second* token after `ITO name`, never
  the first token of a line) — it gets silently dropped with no error,
  producing no symbols at all for that line. This is exactly the failure
  mode the "leave `{Y}` literal instead of substituting empty" decision
  was meant to guard against, except this particular mistake was at the
  *line* level (missing the `ITO` prefix entirely) rather than a missing
  placeholder value — caught by checking the frozen binary's symbol table
  directly after the first conversion attempt produced zero matching
  symbols, not by any error message. Every `FOR` body line that should
  produce a real instruction needs its own `ITO <name>{N}`-style prefix,
  same as the pre-existing convention already used in `yaku.re`'s
  `EPL_SYS_{X}` example. Fixed in both the worked example above and the
  first real conversion (`parser.re`'s `PS_NOP_*` block, verified
  byte-for-byte against the frozen binary).
- Verified via `freeze()`, `trace_callstack_depth.py`, and `diag.py
  --lint` (no new phantom/dead-ref entries beyond the pre-existing
  Pattern R baseline) after every change.

## Motivation

The current `FOR` (see CURRENT BEHAVIOR below) substitutes exactly one
value (`{X}`, the element) plus its index (`{N}`) per iteration. This
breaks down whenever an iteration needs **more than one independent
varying value** — confirmed empirically against a real, large-scale case
in the codebase: the "compute field offset, write a value there" idiom
(`Add El1=lux El2=OFFSET Exit=tmp` / `Write El1=tmp El2=VALUE`) appears
**86 times** across `parser.re` (62) and `saku.re` (24), each occurrence
needing two independent values (OFFSET and VALUE) per repetition — which
the current `{X}`/`{N}`-only model cannot express without manual
unrolling.

## Current behavior (unchanged, stays fully backward compatible)

```
FOR A0 A1 A2 A3 NR
    ITO EPL_SYS_{X}   Move El1=SC_{X}_REF Exit=RA_TMP
    RVOCA EPL_SYS_{X}R  PRELOAD_ARG
```
- `FOR` takes a flat list of element tokens.
- Body = everything indented under `FOR`, repeated once per element.
- `{X}` → the element's own text. `{N}` → its 0-indexed position.
- All 7 existing project usages of `FOR` use exactly this shape and are
  unaffected by anything below.

## New: anonymous count blocks

A token in the `FOR` header line that is purely numeric is a **count**,
not an element — it expands to that many anonymous repetitions instead of
one named element.

```
FOR 4
```
4 anonymous repetitions. The body sees `{N}` (0..3) but has no `{X}` to
substitute (there is no element text for an anonymous repetition).

Numeric and named tokens can be freely mixed in one `FOR` header, in any
order, any number of times:
```
FOR 2 NAME 2
```
→ 2 anonymous reps, then 1 named element (`NAME`), then 2 more anonymous
reps. 5 repetitions total. No restriction on where a count block appears
or how many separate count blocks exist in one header — a count token is
simply "however many anonymous elements belong here," nothing more
special than that.

### Why digits unambiguously mean "count", never "name"

Verified against the existing codebase rule (`loader.py`, the Wave-1
unknown-command name-detection check): a valid Reca identifier must start
with an uppercase letter or `_` (`name[0].isupper() or name[0] == '_'`).
No identifier anywhere in the project starts with a digit (confirmed:
the only digit-leading tokens found project-wide are hex literals like
`0x00`, not names). So "does this header token start with a digit" is not
a new restriction invented for `FOR` — it's already how every name in the
language is required to look. Zero ambiguity, zero new constraint.

## Override mechanism

Indented **below the body**, at one extra indent level, override lines
customize specific iterations.

### Selecting which iteration(s) an override line applies to

There is **one** addressing system, not two. Every slot in a `FOR`
header — named or anonymous — has a position, 0-indexed left to right,
counting every slot (anonymous count-block reps included) in one
continuous sequence. A name is simply a convenient alias for whichever
position it sits at; it is not a separate selector category from a
number. Concretely:

- **By position** (a bare number, or a `LOW-HIGH` inclusive range):
  always valid in any `FOR`, since every slot has a position regardless
  of whether it's also named.
- **By name**: valid wherever that exact name appears as a header token.
  A name that doesn't match any header token is simply unresolved — the
  *same* failure mode as a typo'd name in an all-named `FOR`, not a
  special "wrong mode" case. (There is no such thing as "anonymous mode"
  or "named mode" at the override-line level — only "does this selector
  resolve to something.")
- **Unified counter, 0-indexed**: matches `{N}`'s existing 0-indexing
  exactly, so a position used in an override line is *always* the same
  number `{N}` evaluates to for that same slot inside the body — no
  silent off-by-one between the two.

  Worked example: `FOR A0 A1 A2 A3 NR 3` → `A0`=0, `A1`=1, `A2`=2,
  `A3`=3, `NR`=4, then the 3 anonymous reps from the trailing count
  occupy positions 5, 6, 7. `5-7 > Y=A` selects all three; `A1 > ...`
  and `1 > ...` are two ways of writing the exact same selection.

- **Default line** (`>` with nothing before it): the **default** for every
  slot not otherwise selected by a more specific override line. Written
  with the same `>` marker as every other override line (just with an
  empty selector) rather than a bare `key=value` line with no `>` at
  all — every override-related line contains `>` somewhere, so "does
  this line have a `>`" is the one rule for spotting them, instead of
  needing a second, opposite rule ("absence of `>` means default") that
  the reader has to remember separately.

### Priority when multiple lines could apply to the same slot

A graduated specificity hierarchy, most specific wins:
1. An explicit **name** selector (refers to exactly one slot).
2. A **position or range** selector (may incidentally include a named
   slot's position — see the worked example below — but isn't itself
   tied to that name).
3. The **default** line (`>` alone — applies to everything not caught by
   1 or 2).

This is not three separate rules — it's the same "more specific beats
less specific" principle already established for "named/positional
override always beats a bare default regardless of which is written
first or last in the text," extended one level further. Within the same
specificity level (e.g. two overlapping ranges, or two name selectors for
the same name), the ordinary tie-break applies: the line written *last*
in the source text wins.

Worked example of the overlap case (0-indexed): `FOR 1 3 NAME 5` → total
10 slots, `NAME` sits at position 4 (positions 0-3 and 5-9 are the
anonymous count blocks). A range `1-6 > X=A` spans positions 1 through 6
inclusive — which *includes* position 4, i.e. `NAME` — so this range
applies to `NAME` too, unless a separate, more specific `NAME > X=B` line
exists, which (per the hierarchy above) wins for that slot regardless of
where either line sits in the text.

**Ergonomic consequence (verified):** this overlap behavior means a range
doesn't need to be manually split to avoid a named slot it happens to
contain. For `FOR 2 NAME 4` (0-indexed: positions 0,1 anonymous, 2=`NAME`,
3,4,5,6 anonymous), getting "everything except `NAME`" to one value and
`NAME` to another doesn't require writing two carved-out ranges
(`0-1 > ...` and `3-6 > ...`); one simple, contiguous, overlapping range
(`0-6 > ...`) plus a single `NAME > ...` line to patch that one position
does the same thing. (Often simpler still: if the header's token order
doesn't matter for `{N}`'s use in the body, just write `FOR 6 NAME` in
the first place and avoid the overlap question entirely — the overlap
mechanism mainly earns its keep when the header's order *is* externally
constrained, e.g. `{N}` is used to generate sequential label suffixes
that must follow a specific order.)

### Range syntax

`LOW-HIGH` (inclusive, 0-indexed, same numbering as everything else
above) is part of the design — not deferred. Works identically whether
the positions in range happen to carry names or not; ranges operate
purely in position-space, so there's no special case for "a range that
crosses into named territory."

### Syntax for the `>` separator

Chosen specifically to match the **existing** `SWITCH` macro's own
separator convention (`val1 val2 > dest`, already implemented in
`loader.py`'s switch-mode handler) — not a new symbol, reuse of an
established one. Confirmed `>` is not used as a literal token anywhere
else in Reca syntax (the `Greater` comparison op is spelled as the word
`Greater`, never the bare symbol).

### Multiple keys per override line

`NAME > Y=VALUE` — a `key=value` pair after `>`. (`{Y}` is the first
additional override-only placeholder, chosen as the letter immediately
following `{X}` in the conventional x/y/z unknowns sequence. If more are
needed, the convention is `{Y}`, `{Y1}`, `{Y2}`, ... — bare `{Y}` *is*
the first one (no separate `{Y0}` name exists at all), and explicit
numbering starts at 1 for the second one onward. This avoids the
inconsistency of the first variable being called something different
depending on whether a second one turns out to be needed later — `{Y}`
never has to be renamed to `{Y0}` retroactively.

Requires *zero* implementation changes (confirmed by direct test): the
placeholder-resolution mechanism is already fully generic — it scans
body lines for any `{KEY}` and resolves `KEY` through the override
mechanism uniformly, with nothing hardcoded about `X`/`N`/`Y`
specifically, so `{Y}` and `{Y1}` already work correctly as completely
independent keys in the same `FOR` block today.)

## Worked example: the motivating 86-case pattern

```
FOR C_1 SLOT_E1 SLOT_EXIT
    ITO name_A{N} Add El1=PR_EL1 El2={X} Exit=RA_TMP
    ITO name_W{N} Write El1=RA_TMP El2={Y}
        C_1 > Y=Move
        > Y=C_0
```
`{X}` is the element itself (the offset — `C_1`/`SLOT_E1`/`SLOT_EXIT`,
taken directly from the header list, no override needed for it). `{Y}`
is the value to write: `C_1`'s iteration overrides it to `Move`; the
default line (`> Y=C_0`) supplies `C_0` for everything else (`SLOT_E1`,
`SLOT_EXIT`). Note the explicit `ITO name_A{N}`/`ITO name_W{N}` prefixes
with `{N}`-based uniqueness — a bare `Add .../Write ...` line with no
`ITO <name>` prefix is not a recognized command anywhere in the
pipeline and is silently dropped with no error (caught during real
implementation, see Implementation notes below).
This is the exact shape needed to collapse the 86 manual instances into
`FOR` blocks.

## Resolved: unresolved-selector handling

If an override line's selector (name or number/range) doesn't resolve to
anything that exists in this `FOR` (a name that isn't a header token, or
a position/range outside the total slot count), this is a **loud
build-time error**, not a silent no-op — confirmed by the author,
consistent with this session's established preference elsewhere (e.g.
`SAVE`'s empty-register-list handling) for surfacing misuse rather than
silently swallowing it.

## Resolved: undefined override-only placeholders (`{Y}` with no value
## given for a given slot, neither by override nor by default)

**Leave the literal text `{Y}` in the expanded line — do not substitute
empty string.** Originally designed the other way (substitute empty, to
avoid leaving literal braces in generated Reca code); reversed once the
author pointed out the actual tradeoff: substituting empty string
*also* produces invalid output (e.g. `El2=` with a missing operand, just
as broken as `El2={Y}`), but it additionally *destroys the information*
needed to trace the failure back to its cause — an empty operand
surfaces as a generic, disconnected failure somewhere downstream, while
the literal `{Y}` is a distinctive, greppable marker that points directly
at "this placeholder was never given a value" right where it happened.
Leaving the source text untouched when there's nothing to substitute is
also the more primitive behavior (do nothing extra, rather than invent
empty-string substitution as a special case) — fewer rules, more
information preserved, no real downside.

## Explicitly rejected during design (recorded so they aren't re-litigated)

- **`{Y}`/`{Z}` as shorthand for a combined `{X}_{N}` token.** Checked
  every real `FOR` usage in the project — `{X}` and `{N}` are *always*
  used separately (different purposes: `{N}` uniquifies a generated
  label name, `{X}` supplies a value elsewhere in the same line), never
  combined into one token anywhere. No demonstrated need; would add a new
  symbol to memorize for zero observed payoff. Rejected by the author
  once this was shown.
- **A separate counter scheme that numbers only count-block slots,
  skipping named slots entirely** (so `FOR 2 NAME 2` would number its
  second block `3 4` instead of `4 5`/`5 6` depending on indexing).
  Considered and rejected by the author as needless complexity
  ("separate counter" — a second, parallel numbering scheme to track)
  in favor of the single unified counter described above.
- **Declaring `FOR` header elements as bare numbers instead of real
  names** (e.g. `FOR 0 1 2` written in place of `FOR A0 A1 A2`, to make
  the *element declarations themselves* uniformly numeric). Rejected:
  named header elements are self-documenting at the point of use — losing
  real names in the header itself would force the reader to
  cross-reference position numbers for the actual logic, a real
  readability cost with no upside (this is a distinct question from
  whether a number can be used as an *override selector* for an
  already-named element, which the final design *does* allow — see the
  unified addressing model above; only the *header declaration* syntax
  rejected bare numbers, not override-line selectors).

## Compatibility note

Nothing about this design changes or removes any existing `FOR`
behavior — `FOR A0 A1 A2` (all-named, no override block) is exactly as
valid and means exactly the same thing as it does today. All 7 current
project usages require zero migration.

## Conversion completion (2026-06-20)

Converted all genuine candidates of the original Add+Write field-write
idiom that motivated this whole design. Two corrections to the original
estimate, both caught empirically rather than assumed:

1. **The original "86 instances" count was inflated.** It counted every
   2-line Add+Write *shape* match, without checking whether each was
   part of a genuine repeated group or just an isolated, one-off
   occurrence that happened to share the same shape. Re-counted with the
   correct criterion (2+ consecutive Add+Write pairs sharing the same
   base lux address) and found only **21 genuine candidate pairs**
   remaining after the first conversion — the other ~65 were singletons
   (e.g. `PS_RL_ST`/`PS_NT_ST`/`PS_NT_DONE` in parser.re), each a
   legitimate, distinct, one-off operation that doesn't benefit from
   `FOR` at all (nothing to iterate over). All 21 genuine pairs are now
   converted; the singletons were correctly left untouched.

2. **A handful of groups have an entry-point name that must be preserved
   exactly** (e.g. `PS_MK_LOP_PATTERN`, `LOAD_CI_SKIP_ALLOC`) because
   external code calls into that exact name/address. `FOR` has no way to
   give one specific iteration a different name than the rest, so these
   leading instructions were kept as plain `ITO` lines outside the `FOR`
   block, with only the remaining, internally-named iterations converted.

**Real bugs caught during the conversion** (beyond the ITO-prefix one
documented above):
- A token-read (`RVOCA ... PS_NEXT_SYMBOL` or similar) that must happen
  *before* a value is available for an override (e.g. `PS_RCALL`'s SUB
  token needed for `{Y}=PR_SYM`) has to be moved to *before* the `FOR`
  block entirely — `FOR`'s body is a static template expanded as a whole,
  it cannot pause mid-loop to read a fresh value.
- Auto-generated `{N}`-suffixed names can collide with a manually-named
  sibling line outside the loop that happens to use the same 0-indexed
  suffix (e.g. a hand-written `LOAD_CL_W0` for an offset-0 special case,
  colliding with the `FOR`'s own generated `..._W0` for its first
  iteration). Added a standard verification step for all later
  conversions: scan `_read_re_file`'s expanded output for duplicate `ITO`
  names before running `freeze()`, since a collision doesn't necessarily
  raise an error — the later definition can silently win, masking the
  bug exactly like the empty-substitution case this design already
  guards against elsewhere.

**Unrelated finding, noted but not fixed**: the collision-detection scan
also surfaced 3 *pre-existing* duplicate `ITO` names in `saku.re`'s
`LOAD_RKVA_*` area (`LOAD_RKVA_W0`, `LOAD_RKVA_INC0`, `LOAD_RKVA_J0`) —
not caused by anything in this conversion, not yet investigated. Worth a
look given this is the same general area as the still-open
`LOAD_RKVA_J{N}` fall-through bug from earlier in the project.

Every conversion in this round was verified the same way: direct
inspection of the frozen binary's field values (not just "freeze()
didn't crash"), plus `trace_callstack_depth.py` and `diag.py --lint`
(no change from the pre-existing 163-issue Pattern R baseline) after
every batch.
