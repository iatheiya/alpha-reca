# PLAN: the declaration family, second pass

## Why this exists

The 2026-07-04 unification (`NEW`/`SEAL`/`OWN`/`FREE`/`SET`/`ALIAS`,
recorded in DECISIONS.md) landed, but re-examining it against the real
code found three things it got wrong, plus one live bug it explains.

Everything below was measured, not assumed. Where a claim has a number,
that number came from the project as it stands.

## What the measurements said

### Most modifiers are dead

Counted across every `.re` file:

| modifier    | uses |
|-------------|------|
| `\| OWN`     | 300  |
| `\| ANON`    | 8    |
| `\| NOLINK`  | 7    |
| `\| SET=`    | 0    |
| `\| SEAL`    | 0    |
| `\| FREE`    | 0    |
| `\| ALIAS`   | 0    |

`| SET=` is dead for a reason that matters: plain `SET X 5` **already
creates the name** if it does not exist (`loader.py`: `if name not in
self.symbols: alloc`). The modifier was a second way to say something
that needed no second way.

`| FREE` is the one dead modifier that stays — it is deliberate
groundwork for the per-thread-personal work (Pattern W /
PLAN_SCHEDULER.md), the symmetric opposite of `OWN`.

### SET does not absorb SEAL

An earlier attempt argued they differ only by *when* they run (creation
vs. later rewrite). That is wrong — `SET` creates too, so timing does
not separate them. The real difference is *what* is written:

- `SET X Y`  → `word(X) = word(Y)` — copies Y's **value**
- `SEAL X Y` → `word(X) = addr(Y)` — writes Y's **address**

`SEAL` also explicitly rejects integers (`not _is_integer(ref_name)`),
so it cannot write a literal at all. Two different actions; neither
reduces to the other.

### BLOCK is redundant everywhere except one place, and that place is broken

`loader.py` states it outright: `BLOCK name N = NEW name + NEW name_001
+ ... + NEW name_(N-1)`, with `BLOCK_ELEM_SIZE = 2`.

- Every `BLOCK` whose indexing is visible is indexed by **1**, not 2
  (`IRIS_BUF` + `RA_IRIS_SIZE`, `DS_START` + `RA_DS_POS`, `BS_TOKBUF`
  + `RA_LOAD_TLEN`). Half of each is unused: `BLOCK BS_TOKBUF_000 256`
  occupies 512 luces and addresses 256.
- The genuinely pair-shaped structure — the symbol table, 2 words per
  slot — uses **`SPAN`**, not `BLOCK`
  (`SPAN BS_HTAB_000 131072  /65536 slots * 2 words`). Pairing is
  already expressed by writing the product.
- `BLOCK`'s per-element aliases have exactly one user: `parser.re`
  writes 45 pre-computed hash entries as `SET PBKT_011 <n>` .. and
  **here BLOCK is doing its job correctly** — `PBKT` is a hash table
  read by `HT_LOOKUP` at `slot*2`, and BLOCK's stride is 2:
  `PBKT_001` = base+2, `PBKT_002` = base+4. The aliases *are* the slots.

### The live bug

Those 45 writes do not land in the table.

    PBKT_000 = 1358197     (block base)
    PBKT_001 = 1358199     (base+2, correct)
    PBKT_011 = 3698        (nowhere near the block)

`SET PBKT_011 <n>` runs before `BLOCK PBKT_000 256` creates that alias.
`SET` auto-creates the name, `BLOCK` later finds it taken and silently
skips it. The author intends to fill slot 11 of the hash table and
writes into unrelated memory instead. 45 entries, all of them.

This is not a reason to keep BLOCK — it is a reason the whole
name-the-slots approach is fragile: it depends on declaration order
that nothing enforces.

## The shape

    NEW   X                  one lux
    SPAN  X Y N              Y elements of N luces each (N defaults to 1)
    SET   X value            write X's word; creates X if missing
    SEAL  X Y                write addr(Y) into X's word; creates X if missing
    LAY   X pos value        write into element `pos` of span X
    LINK  src rel tgt        connect
    ALIAS X Y                a second name for the same address

Modifiers: `| OWN` and `| FREE` only.

`BLOCK` is deleted.

### Why LAY

`SPAN X Y N` covers the memory but not addressing a slot by name — the
only thing BLOCK's aliases were for. `LAY` covers it by position
instead, which kills the aliases *and* the bug: writing to `base+22`
cannot lose a race with a name that does not exist.

Checked before choosing: `LAY` is unused anywhere in the project, and
fits the register — the command words split into thematic ones (`Lux`,
`Lumen`, `Nova`, `Voca`, `Nexo`) and plain English ones (`NEW`, `SET`,
`LINK`, `SPAN`); `LAY` sits with the second group, next to `SET`. It is
not the `REF` case (jargon abbreviation, rejected 2026-07-04) — `lay`
is an ordinary verb that says what it does.

### Why SPAN X Y N and not a modifier

`SPAN X Y | NAMED N=2` was considered and rejected. Naming elements has
one user in the whole project (the 45 `PBKT` lines), and those are
data, not names. A modifier for one case is the same mistake `| SET=`
(0 uses) and `| SEAL` (0 uses) already are.

Positional matches `LINK src rel tgt`, introduces nothing, and reads as
what it is: this many, of this size.

Note: `SPAN X 131072` and `SPAN X 65536 2` allocate the same region.
That is fine — N defaults to 1, and writing the product by hand stays
legal. The author already writes the product in comments today
(`/65536 slots * 2 words`, `/256 slots * 5 luces/node`); `SPAN X Y N`
just lets the code say what the comment says. One of those has a
multiplier of **5**, which is why this is `N`, not a `| 2` flag.

### Not adding: a multi-value LAY body

`LAY X 11` with an indented body was considered. There is no case in
the project that needs it — the 45 `PBKT` writes go to scattered slots
(11, 12, 13, 19, 29, 31...), so they are 45 separate writes either way.
Adding the form now would be building for a case that does not exist,
which is the exact habit this whole pass is undoing.

## Status

Done. All five stages.

| | |
|---|---|
| `LAY X pos value` | added, 45 uses |
| `SPAN X Y N` | added; N defaults to 1, existing `SPAN X N` unchanged |
| `BLOCK` | deleted — 16 uses converted to `SPAN` |
| `\| SET=` `\| SEAL` `\| SEAL=` `\| ALIAS=` | deleted, with their four tests |
| `\| OWN` `\| FREE` | kept |

**The PBKT bug is fixed and verified.** The 45 pre-computed hash entries
now land inside the table:

    slot 11 -> addr 1358129 (was 3698, outside the table)  value matches
    slot 12 -> addr 1358131                                value matches
    slot 95 -> addr 1358297                                value matches

`LSYM_000` got the same treatment (`SPAN LSYM_000 512 2`) — it is also an
`HT_LOOKUP` table with 2-lux slots and had the same latent fragility.

**Measured effect of deleting BLOCK:**

- symbols: **76001 → 7029**. About 69,000 generated per-element aliases
  (`X_001`, `X_002`, ...) existed solely because BLOCK named every lux it
  allocated. One place used them; that place was broken.
- luces touched: 5.35M → 5.21M. Every BLOCK addressed by 1 was allocating
  double and using half.

Self-hosting held at 19/42 throughout; 11/11 tests, lint clean, full
check suite clean.

### A mistake worth recording

Partway through Stage 5 the four modifier tests failed, and the first
instinct was to treat "it has a test" as "it must stay" and back out. That
was wrong twice over: the tests existed to check a feature that had just
been decided against, so failing was the correct outcome, not a signal to
stop. A test is evidence of intent, not a veto. They were removed with the
feature.

The other trap in this stage was self-inflicted: `wax.re` had been left
half-converted to a cache design that was then abandoned, so it still
referred to `TIDE_CLEAN_BASE` after the buffer had been renamed
`TIDE_CACHE_000`. That broke self-hosting to 1/42 and cost a long detour
through LAY and SPAN, which were both innocent. Abandoned work has to be
reverted completely or not at all.

## Out of scope

`NEXO` — it already absorbed five `YAKU_NEXO_*` commands, which was the
right instinct, but its contract ("build a packed string template **and
optionally link to Yaku**") wires a general shape to one consumer:
`Yaku_name`, `Yaku` and `ForType` appear inside the macro itself. Whether
that belongs at the call site instead is a real question and needs its
own pass. Recorded in DECISIONS.md.
