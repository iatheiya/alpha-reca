# Name= token migration — state, blocker, and what remains

Goal: make a naming command's own label a keyword operand (`Name=`) instead of
a positional one, so that the name becomes **optional** and an instruction that
nothing ever jumps to needs no invented name at all.

Once the positional form is gone, three things follow for free:

- **Order independence.** Positional arguments start filling at MA1 and MA0
  belongs to `Name=` alone, so `Name=` may appear anywhere on the line. This
  cannot be achieved while the positional form still exists: a single-pass
  streaming parser cannot tell whether the first bare token is a name or an
  operand without lookahead. Order independence is a Phase C property, not a
  Phase A one, and needs no lint rule to enforce it.
- **A smaller bug surface.** BUGS.md Pattern Z (a duplicate name silently
  skipping an allocation) can only happen to *named* things. Steps that never
  needed a name cannot collide.
- **Block anonymity disappears as a concept.** `CHAIN _` and `CHAIN NAME | ANON`
  exist only because a NOVA cannot be nameless on its own; the block was forced
  to own a naming policy that was never its job. When NOVA can be anonymous,
  both forms become a special case of the general mechanism and are deleted,
  along with `LD_CH_FULLY_ANON`, its 0x5F detection in macros.re, and the anon
  branches in `_parse_iris_mode` / `_auto_name`.

## Phase A — DONE, both loaders accept `Name=`

**Python.** `_parse_record` already separated bare arguments from slot keywords,
so it was enough to add `Name` to `_REC_SLOTS` and normalise it back into
`args[0]` at parse time. No wave handler was touched: downstream, the new form
is indistinguishable from the old. The shared split lives in one function,
`_split_args_slots`, used by both `_parse_record` and the macros.re scan.

Because the normalisation is command-agnostic, every macro-class command
(`JEQ`, `JZ`, `CLEAR`, `NOVAE`, `EMIT`, ...) was covered automatically.

**Reca.** Two paths, one change each:

- `LOAD_READ_NAME` (wax.re) = `BS_READ_TOKEN` + `BS_TOKEN_VALUE`. The latter
  already strips a `key=` prefix and recomputes the hash, and returns a token
  containing no `=` untouched, so the positional form passes through unchanged.
  13 primitive handlers call it instead of `BS_READ_TOKEN`.
- `LOAD_RKVA` (the generic macro argument reader) gained a `Name=` branch.
  Macros read no tokens themselves — they all receive arguments through this
  one reader — so a single branch covered every macro-class naming command.

Verified: the helper is exercised 3662 times per self-hosted run and strips
3662 times. It is correct.

## Phase B — INCOMPLETE and currently blocked

4228 top-level naming lines are in the new form. **639 are not**, dominated by
`RVOCA` (486), then `NOVAE` (47), `EMIT` (22), `RCALL_AT` (19), `ALLOC_TO` (17),
`EMITI` (12), `WALK_ONE` (11), `PUTBYTE` (9), `LINK_OP` (5), `NOP` (4). Both
forms are legal in Phase A, so a mixed corpus builds, but Phase C cannot start
until the count reaches zero.

Not yet converted at all: the 404 indented lines inside blocks, the ~18 line
synthesis points in `loader.py` (`_expand_indent`'s `_auto_name`, the SWITCH and
ALLOC_RAW expansions, and the NOVA/RVOCA emitters), and `gen_compiler.py`.

### The blocker: SEAL name-pool corruption — RESOLVED (two separate defects)

**1. The buffers overwrote their own SPAN header.** `SK_SEAL_NAMEPOOL_PTR` and
`SK_NEWREF_NAMEBUF_BASE` were initialised with `Move`, which dereferences the
SPAN to its own base address — so the first byte of the first name landed on the
SPAN's self-reference word and destroyed it. From there the pointer was
near-zero and subsequent writes walked over the register file, eventually
setting `RA_FLUX_BOTTOM` to 69 (ASCII `'E'`) and tripping the allocation bounds
guard `AC_G_HALT` — which was right to halt: the limit it read was garbage, not
exhausted. Both now use `Add El2=C_1`, starting past the header.

**2. `BS_TOKEN_VALUE`'s copy loop was unreachable.** Its conditional
`BS_TV_CLOOP_J` had `next=0`, so its false branch fell through by physical
adjacency — landing on `BS_TV_HLOOP_J` in the hash section below. After finding
the `=`, control skipped the copy entirely: `RA_LOAD_TLEN` was never updated and
the token came back unstripped. The two other conditionals in the same routine
carry an explicit next (`BS_TV_LOOP_J -> BS_TV_BADDR`, `BS_TV_EQC_J ->
BS_TV_INC`); the difference is that those are followed in source by an ordinary
`NOVA`, while this one was followed by a `CHAIN` block, which does not become the
fall-through target. A plain `NOVA BS_TV_CGO Jump Exit=BS_TV_CSRC` between them
restores the link.

This second defect is **not** a migration artefact. `BS_TOKEN_VALUE` is the
shared `key=value` prefix stripper — `El1=` / `El2=` / `Exit=` parsing calls it
too — so the self-hosted loader has never been able to strip a keyword operand,
which is part of why it only ever processed a fraction of the project. Moving
the name to `Name=` made that path mandatory, and therefore visible.

Both defects are of one kind: silent layout errors that no structural check
could catch, because in both cases the graph is *correct* — a pointer that
legitimately holds a base address, and a conditional that legitimately has no
explicit next.

Measured after both fixes: self-hosted run **16.8M steps / 1339 htable
entries**, against 3.5M / 367 before any of this work began.

### Order of work when this resumes

1. Reserve a header element for `SK_SEAL_NAMEPOOL` so the pointer starts past
   the SPAN's own word, mirroring htable.re.
2. Find where the name buffer receives an unstripped token.
3. Finish the 639 remaining conversions, then the block bodies, the loader.py
   synthesis points, and gen_compiler.py.
4. Phase C: delete the positional form from both parsers; positional arguments
   then start at MA1, `Name=` owns MA0, anonymity becomes expressible, and the
   block-anonymity machinery listed at the top is removed.

## Resolved: LOAD_RKVA_PSTART was a broken contract, not a duplicate

`LOAD_RKVA_RESOLVE` states its requirement in its own header: *"for the Symbol
path, the caller must set SK_INTERN_SLOT to its own real, final destination
address before calling this"*. It matters because `SK_MA_RESULT` is only an
intermediate register — every caller copies it into its own destination
afterwards, so backfilling `SK_MA_RESULT` itself would land too late.

Every keyword argument honours that contract: `LOAD_RKVA_ELSI2` / `ELSI3` for
`El1=` / `El2=`, `LOAD_RKVA_EXSI` for `Exit=`. **Positional arguments did not.**
Their slot is set by the anonymous `SWITCH SK_POS_SLOT` block, and nothing could
reach it: the block has no name to jump to, and its only physical predecessor is
an unconditional `Jump`, which always leaves via `Exit` and never falls through.
All four positional paths jumped straight to `LOAD_RKVA_POS`, which sits *after*
the switch.

So a forward-referenced positional macro argument was resolved with whatever
`SK_INTERN_SLOT` the previous operation had left there, and its backfill wrote
into that stale address. `LOAD_RKVA_PSTART` — a named no-op adjacent to the
switch — gives the four paths something to enter, restoring the intended route.
It is not a duplicate of anything; nothing else sets that slot for positional
arguments.

The redirect is in place. It cannot be demonstrated by the interned-name count
right now, because that count is dominated by the SEAL name-pool corruption
above; the justification is the contract, which is checkable by reading, not by
a counter. Worth re-measuring once the SEAL bug is fixed.
