# The `_REF` pattern and its `Idem`-based resolution — a record

Written after migrating the first `_REF` group (`SK_WB_*_REF`, wax.re's
own word-bootstrap block) to `Idem` (Aspect #25). Kept here so the next
person doesn't have to re-derive the reasoning from scratch. See
`BUGS.md` Pattern Z for the full discovery trail; this file is the
condensed, load-bearing summary.

---

## Why `_REF` existed at all

`Move`/`Equal`/every other Aspect that reads an operand dereferences it
(`aether[a1]`, not `a1` itself). There was no way to get a symbol's own,
already-known-at-build-time address as a plain value without first
building a dedicated holder lux whose *word* is set to that address
(`SEAL X_REF X`), then dereferencing the holder instead of X directly.

`Idem` (Aspect #25, `aspects.re`) removes the need for the holder: it
copies its own operand *without* dereferencing it, so `Idem El1=X
Exit=Y` gives `X`'s address directly, in one step, with nothing
pre-declared.

## The two orthogonal usage shapes `_REF` was covering

These are genuinely different problems that both happened to use the
same `_REF` mechanism as a workaround. `Idem` resolves them differently:

### Shape 1 — "I just need this address once, to write it somewhere"

Example (the word-bootstrap block, before this session's fix):
```
Move El1=SK_WB_ALIAS_REF Exit=RA_HT_LID
```
Fix: use `Idem` directly against the bare name, no holder at all:
```
Idem El1=ALIAS Exit=RA_HT_LID
```
One instruction either way — `Idem` strictly replaces `Move` here, no
holder lux needed, no reduction in instruction count (it wasn't costing
extra instructions at the *use* site, just an extra *declaration* that's
now gone).

### Shape 2 — "I need to compare something against this address"

Example (the declare-only allowlist, before this session's fix):
```
JEQ LOAD_CU_DSL SK_CMD_ADDR SK_WB_SEAL_REF LOAD_CU_BME1S
```
`Equal` (which `JEQ` expands to) dereferences *both* operands — there is
no way to make it compare a live value against a raw, undereferenced
address directly. `Idem` cannot replace `Equal`'s own dereferencing;
it can only replace how the *comparison target* gets its address:
```
Idem El1=SEAL Exit=SK_TMP3
JEQ LOAD_CU_DSL SK_CMD_ADDR SK_TMP3 LOAD_CU_BME1S
```
Two instructions instead of one `Move`-based `_REF` declaration reused
across every comparison that needed it — a real trade-off (more
instructions at each *use* site) in exchange for no standing
declaration and no "do I need a `_REF` here" bookkeeping. For a target
used at only one comparison site this is a straightforward win; for one
used at several sites (e.g. `SEAL`, compared in two places this
session) it costs one extra instruction per site instead of one shared
declaration — still chosen over reintroducing a holder, per the
author's own explicit call (`_REF` "must disappear").

## What migrated, what's left, as of this writing

Migrated (Shape 1 + Shape 2 mixed, `wax.re`'s own word-bootstrap block
and declare-only allowlist): `SK_WB_ALIAS_REF`, `SK_WB_SPAN_REF`,
`SK_WB_SEAL_REF`, `SK_WB_OWN_REF`, `SK_WB_FREE_REF`, `SK_WB_CLEAR_REF`,
`SK_WB_NOP_REF`, `SK_WB_RVOCA_REF`, `SK_WB_RREDI_REF`, `SK_WB_JEQ_REF`,
`SK_WB_JZ_REF`, `SK_WB_ALLOCTO_REF` — all removed, all call sites now
use `Idem` directly.

Not yet migrated, confirmed **zero** `NOVAE` dependency (safe, same
technique applies directly): `CHAIN_MOD_NOLINK_REF`/`CHAIN_MOD_ANON_REF`
(`macros.re`), `RA_TW_LUX_REF`/`RA_TMP2_REF`/`RA_BYTE_REF`
(`registers.re`), `SC_A0_REF`/`SC_A1_REF`/`SC_A2_REF`/`SC_A3_REF`/
`SC_NR_REF` (`yaku.re`).

Not yet migrated, **some** `NOVAE` dependency (riskier — `NOVAE`'s own
Python-side expansion reads these as a literal operand to synthesize a
*runtime*-constructed NOVA; using `Idem` there would need `NOVAE`'s own
mechanism, not just the Reca source, to change too): `RA_ALLOC_RESULT_
REF`/`RA_ALLOC_COUNT_REF` (`alloc.re`), `C_7_REF` (`constants.re`),
`RA_LINK_REF`/`RA_C0_REF`/`RA_SP_REF`/`RA_RCA_TMP_REF`/`RA_RCA_TMP2_
REF`/`RA_FRAME_SIZE_REF`/`RA_SR_LUX_REF`/`RA_SR_REL_REF`/`RA_SR_OUT_
REF`/`RA_SR_LUMEN_REF`/`RA_SR_OFFSET_REF` (`registers.re`).

## Update — a third, genuinely distinct shape found; not a migration gap

Investigated the 13 `NOVAE`-related survivors directly rather than
migrating them on the same assumption as the other two shapes. They
are not a third instance of "working around `Idem`'s absence" -- they
solve a different problem `Idem` does not address.

`NOVAE`'s own Python-side expansion (`loader.py`, `_wave2_line`)
hardcodes `Move` (not parameterizable via the `NOVAE ...` source syntax)
to resolve each of `El1=`/`El2=`/`Exit=` into the *synthesized*
instruction's own operand fields. This one `Move` step legitimately
serves two different needs depending on what's passed:

- A normal register (`El1=RA_MA1`): the synthesized instruction's own
  operand should be *whatever address RA_MA1 currently, dynamically
  holds* -- exactly what a single dereference gives.
- A `_REF` holder (`El1=RA_C0_REF`): the synthesized instruction's own
  operand should be a *fixed, known-at-compile-time constant*
  (`addr(C_0)`) -- also exactly what a single dereference of the
  holder gives, since the holder's own word was set to that address.

Both cases correctly use the same one-dereference `Move` -- they are
not the same bug `Idem` fixed elsewhere.

## Final update — NOVAE extended with its own "Idem:" mode; all `_REF` gone

The author's own follow-up question ("could NOVAE be made more general,
not tied to Move alone?") resolved the above cleanly, rather than
leaving it as a permanent exception. `_wave2_line` now recognizes an
optional `Idem:` prefix on any of `El1=`/`El2=`/`Exit=`
(`El1=Idem:RA_C0` instead of `El1=RA_C0_REF`) -- when present, that one
operand resolves via `Idem` (zero dereferences) instead of the default
`Move` (one dereference), chosen explicitly per operand rather than a
`_REF` holder faking the zero-dereference case through `Move` alone.
The far more common case (a dynamic register's current value) is
untouched -- `Move` remains the default, nothing about existing,
unprefixed `NOVAE` calls changed.

Verified directly, not just by absence of build errors: drove
`CLEAR_SPN` (`macros.re`'s own `CLEAR` implementation, `NOVAE CLEAR_SPN
Move El1=Idem:C_0 Exit=RA_MA1 At=RA_MA0`) through the real interpreter
end to end -- confirmed the synthesized instruction correctly holds
`op=Move, e1=C_0's real address, exit=<the real target>`, and running
*that* instruction correctly writes `0` into the target.

All 25 remaining `_REF` symbols (the 13 `NOVAE`-dependent ones, plus 3
more found only by grepping `wax.re` directly -- `RA_LINK_REF`/
`RA_C0_REF` used via plain `Write`/`Add`, not `NOVAE` at all, missed by
the earlier `NOVAE`-usage-count sweep since it only checked known `_REF`
names project-wide, not every file) migrated and removed. Confirmed:
zero `_REF` declarations or references remain project-wide (grep
clean, excluding this file's own historical description and one
`BUGS.md` comment referencing the pre-`Idem` approach as history).
