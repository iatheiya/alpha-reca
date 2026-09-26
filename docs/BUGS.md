# BUGS.md — Bug Patterns and Debug Lessons

Recurring bug patterns, known-open issues, and debugging lessons from bringing
the self-hosted loader (`LOAD_MAIN`, `wax.re` and friends) to parity with
`loader.py` (the source of truth). The value is in the patterns, not the
individual bugs.

## How to use this file

- **Hunting a bug:** skim "Recurring patterns". Most new bugs turn out to be
  one of these classes again. Each entry has symptom, cause, fix and a
  detection method.
- **Picking up work:** read "Current status" and "Open issues and future work".
- **Before writing a new diagnostic script:** read "Debugging and tooling
  lessons". Most tracing mistakes made so far are listed there.
- **Code comments cite entries by name** ("see BUGS.md Pattern Z", "Pattern N",
  "Pattern W", "forty-third round", and so on). Every such name has a heading
  or a tagged paragraph here. Keep the names stable. Round numbers ("thirty-fourth
  round" and so on) are the per-session steps of the Pattern Z investigation. They
  are kept as tags inside Pattern Z so that external references still resolve.
- **Names drift.** Most of this log was written when `macros.re`, `SEAL`,
  `NEWREF`/`SETREF`, `NEWSET`, and `diag.py` still existed. Since then,
  `macros.re` content has been merged into `commands.re` (by way of
  `expand.re`/`synth.re`), `SEAL` has been retired as a subset of `SET` (command
  registration now uses `WAX_CMD`), `NEWSET` lines were renamed to `SET`, and
  `diag.py` has largely been replaced by `iris.py` (`iris --lint`). Several
  `check_*.py` scripts named below may also have been folded into `iris.py`. Check
  `TOOLING.md` for current tool names. The lessons still apply.

---

## Current status (honest summary)

**Self-hosting has not been achieved.** "Self-hosting" means `LOAD_MAIN`
building the whole project itself, correctly, as the authoritative source of
truth (ROADMAP B6).

The number usually quoted, "N/42", is only `SK_FIDX`, a file counter. It
advances when a file is swept past, even when half of that file's work was
skipped through dead references. Over the sessions it went 4 -> 33 -> 14 ->
40 -> (many swings during Pattern Z) -> "42/42" several times. Every 42/42
later turned out not to mean a correct build:

- The graph-equivalence check (`check_graph_equivalence.py`) found about 1900
  garbage op-primitives, which led to the `htable.re` bugs (Pattern Z,
  forty-third to forty-fifth rounds). After those fixes, 681 structural
  mismatches remained.
- Later, fixing `_discover_nova_naming_commands`
  (`HASH_STEP` case) stopped `_nova_lux` from repointing symbols to orphan stubs
  (re-allocations went from 58 to 0). References that used to go nowhere now
  run real code. Interned names doubled (1339 -> 2789), steps went from 16.8M to
  43M, and `SK_FIDX` dropped to 40: the old 42/42 had been counting files swept
  past with half their work skipped, not files built.
- True fixed-point self-compilation (LOAD_MAIN's output being equivalent to
  Python's) has never been checked. ROADMAP describes LOAD_MAIN as running
  authoritatively on "0/42 today".

Latest measurement (the corpus is now 82 files): lint and Pass2 are clean, and
the file counter reaches 82/82. `iris.py --graph-equivalence` reproduces 1023
of 1465 named NOVA nodes correctly: 70 wrong (mostly the Next slot), 372 husks
(interned but never wired), and 313 names missing from SK_HTAB. The build is
still not equivalent, so treat 82/82 the same way as the old 42/42. These
numbers shift slightly with file order (the self-hosted build is still
order-sensitive): the same sources gave 1012/82/371 in a flat layout.

Last fix before that measurement: the self-hosted `SET X Value=<int>` also added
a `Constant -> Yaku` lumen to X through `ADD_LUMEN`. X is a 1-cell interned name
with no lumen room, so the scan ran into the neighbouring name cells and wrote
word=Yaku into them. `EXP_CMD_NOVA` then "reused" Yaku as the body of those
cells and overwrote `SYNTH_YK_LOFF`. `loader.py` records no such lumen and
nothing reads it, so the fix removed it. General lesson: **never call
ADD_LUMEN on a lux that was not sized for lumina**.

---

## Recurring patterns (named)

### Pattern 1: Phantom Labels (EXIT/LINK target never defined)

- **Symptom:** a name used as a jump, Exit or LINK target has no matching NOVA
  definition. The lux keeps `op=0` and terminates execution when it is reached
  ("no handler").
- **Instances:** `LOAD_CU_SKIP`->`LOAD_CU_SKPL`, `LOAD_CB_SKL`->`LOAD_CB_SKLS`,
  `LOAD_RB_LOOP`->`LOAD_RB_RL`, the malformed `LOAD_RB_LC_JMP2LOAD_RB_`; five
  copy-paste typos in `lexer.re` (`BS_RT_SL2`->`BS_RT_SL2B`,
  `BS_RT_BLKSK`->`BS_RT_BLK`, `BS_RT_SLCK`->`BS_RT_SLRB`,
  `BS_RT_SLBL`->`BS_RT_SLBK`, `BS_BLK_SL2`->`BS_BLK_SL2B`), which crashed
  comment detection on a lone `/`; `P0_ALLOC_EPBUF` (stale after a refactor to
  `ALLOC_RAW`, fixed with a `NOP` landing marker); `SEAL SWITCH_RESOLVE_NAME
  SW_RN_IMPL`, where the body was actually labelled `SW_RN_ANON`.
- **Data-name variant:** `ASCII_EQ` (never declared; the real constant is
  `EQUALS`, value 61). Wave-1 gave it a generic stub with an arbitrary value, so
  `BS_TOKEN_VALUE` silently never stripped `key=`. An undefined data name does
  not crash. It compares against garbage.
- **Lesson:** a one-character typo creates a phantom that is invisible when
  reading the code. Names must match exactly.
- **Detect:** lint for Exit/LINK targets whose `aether[addr+1] == 0`
  (`iris --lint`, formerly `diag.py --lint`, reports "PHANTOM NOVAs" and "DEAD
  REFERENCES"; see also `check_undef_targets.py`). Quick check:
  `[a for a in nova_addrs if aether[a+1] == 0]`.

### Pattern 2: Name Conflicts Between Files

- **Symptom:** the same label in two files. One overwrites or shadows the other
  on intern.
- **Instances:** `AC_WRCUR` (`alloc.re` vs `arena.re`, renamed to `ACL_WRCUR`);
  `SC_LOOP/SC_INC/SC_DONE/SC_RET` in `comments.re` (renamed to `CMT_*`). Later
  instances of the same class: Pattern Y (`NEXO`) and the `LOAD_CN_INTERN`
  collision (Pattern Z).
- **Rule:** prefix local labels with the file or module (`LOAD_*` for wax.re,
  `CMT_*`, `ACL_*`). Only global names go without a prefix.
- **Detect:** the duplicate-label check (Pattern 10).

### Pattern 3: JZ Builder Writes to a System Lux Address

- **Symptom:** the JZ builder's `ALLOC_LUCES` returned address 328, which is
  where `BS_FILE_COUNT` lives. Its word (the file count) was overwritten.
- **Cause:** system luces (`BS_FILE_COUNT`, `K_CURSOR`, ...) sit in roughly
  addresses 80-400, and the bump allocator can reach them.
- **Workaround:** `loader.py` restores `a[bs_file_count] = len(all_files)` after
  LOAD_MAIN. The proper fix was never done: a guard in `ALLOC_LUCES`
  (`new_bump < SYSTEM_RESERVED_TOP` -> halt) or placing system luces above the
  initial bump.

### Pattern 4: SETREF Dispatch Bug (SET vs SETREF)

- `LOAD_DISPATCH_LINE` sent `'S'+'E'` to `LOAD_CSS_SETCK`, which checked
  `tlen==3` for SET. Anything else fell into `LOAD_CMD_UNKNOWN` by accident.
  Fixed with an explicit `NOLINK` path (`LOAD_CSS_SEUCMD`).
- **Lesson:** every dispatch branch must handle all cases explicitly. A missing
  fall-through is undefined behaviour. The same first/second-byte dispatch later
  missed `SPAN` (Pattern 15) and needed care for `FOR`/`FREE`.

### Pattern 5: K_CURSOR / K_WATERMARK Corruption

- **Symptom:** after LOAD_MAIN, `K_CURSOR.word` has the wrong value, so the next
  `ALLOC_LUCES` returns occupied space.
- **Workaround only:** `loader.py` does `a[k_cursor] = k_cursor + 1;
  a[k_cursor+1] = loader._bump` after LOAD_MAIN.
- **Status:** root cause never found. It was not re-verified after the Pattern Z
  work (Wave 0.5, backfill, word bootstrap). It may still be latent or may have
  been a symptom of something since fixed. Check directly before assuming either.

### Pattern 6: Write Protection as a Hack on Top of a Hack

- LOAD_MAIN wrote an RVOCA address and then `RA_LINK`'s address into
  `NEWREF.word`, which broke dispatch (`op=0`). Runtime write protection in
  `loader.py` was either too strict or too weak, and every tightening opened a
  new corruption path.
- **Lesson:** Python-side write protection is a diagnostic, not a fix. If
  LOAD_MAIN writes garbage, the bug is in the Reca code. Find the line that
  produces the bad write. At most, keep protection for invariants that must
  never change during pass 2 (`K_CURSOR`, `K_WATERMARK`, `BS_FILE_COUNT`).
  Treat this as the general "symptom -> hack -> new symptom" anti-pattern.

### Pattern 7: Tracking Error — Wrong Phase Trigger

- A trace used `oc[0] == 71` to detect the opening of `wax.re`, but 71 was
  `linux_generic.re` and wax.re is 72. Every token analysed as coming from wax.re
  came from the wrong file.
- **Lesson:** capture the real file name on `openat` (decode the path from the
  aether). Never infer it from a counter.

### Pattern 8: LOAD_MA_READARG and RA_LOAD_BYTE (open)

- **Historical hypothesis:** `LOAD_MA_READARG` skipped reading MA1 because
  `RA_LOAD_BYTE == LF`. The "root cause chain" built on it (NEWREF args shifting
  one line, cascading into `op=0`) was **never confirmed**, and later sessions
  found the real blockers elsewhere.
- **Status:** superseded. The function no longer exists; it was replaced by
  `LOAD_READARG_KV`/`LOAD_RKVA_RESOLVE`, and only stale comments mention the old
  name. The heading is kept only for references.

### Pattern 9: Dereference Depth Mismatch (address needed, word given)

- **Symptom:** a comparison or move silently uses a symbol's *word* where its
  *address* was meant. It only works by coincidence while the symbol is still
  self-referential, which command and macro names stop being once they are
  wired to a handler.
- **Cause:** operand dereference depth. `Move`/`Equal`/`Add`/... dereference
  once (`aether[a1]`). `Equal`/`JEQ` dereference **both** operands (`_cmpeq`:
  `aether[a1] == aether[a2]`). `Write`/`Read` dereference twice.
  `JEQ SK_CMD_ADDR SEAL ...` therefore compares against `SEAL`'s handler
  address, not `SEAL` itself.
- **Instances:** all 12 checks in `LOAD_CMD_UNKNOWN`'s declare-only allowlist
  (`ALIAS/SPAN/SEAL/OWN/FREE/CLEAR/NOP/RVOCA/RREDI/JEQ/JZ/ALLOC_TO`); fixing them
  moved `SK_FIDX` from 6 to 28 (twenty-third round). Others: the word-bootstrap
  block's `Move El1=NEWSET Exit=RA_HT_LID`; `LOAD_CU_SEALCK`; `SWITCH_INDCK`/
  `SN_INDCK` comparing `LD_INDENT_DEPTH` (a `NEWREF`-style alias whose word is
  `addr(SK_IND_DEPTH)`) against `C_0`, a permanently dead check;
  `SPAN`-allocated buffers (`RA_FOR_IRIS_IDX_BASE`, `RA_FOR_OV_HEAD_BASE`,
  `SK_SPAN_NAMEBUF`, `SK_BOOT_NAMEBUF`), which are not self-referential, so
  address arithmetic on them needs `Idem` first.
- **Not instances:** comparing a byte register against a value constant (`LF`,
  `SP`, `ASCII_*`, `C_N`), and `LOAD_CU_CHCK`/`JZCK`/`JEQCK`, which
  deliberately compare an already-dereferenced word against a name's word.
- **Fix:** `Idem`, native Aspect #25 (`aether[exit] = a1`, zero dereferences),
  completes the ladder at depth 0. It replaced the `_REF` holder workaround
  (`SEAL X_REF X`, as in `C_7_REF`, `CHAIN_MOD_NOLINK_REF`, `SK_WB_*_REF`).
  Migration record: `REF_TO_IDEM.md`. `NOVAE` operands needed an `Idem:`-prefixed
  operand syntax because their targets are dual-purpose.
- **Detect:** `check_deref_mismatch.py` flags El1=/El2=/JEQ comparisons against a
  non-self-ref registered macro name. A project-wide sweep of all 182 JEQ lines
  and every raw `Equal`/`Move`/`Read` found no further instances after the 12.

### Pattern 10: Duplicate Label Definitions

- **Symptom:** `loader.py`'s "already exists -> no-op" rule (needed for forward
  references) also swallows real duplicates. The second definition's content is
  **never allocated**, and auto-chain falls through into whatever comes next.
- **Instances:** `LOAD_RKVA_EL` (a `JEQ` whose own name equalled its
  destination, so its Equal was never allocated), `LOAD_RL_EOF2`, `LOAD_CL_RRET`
  (3 of about 2973 labels); `LOAD_CN_INTERN` reused as an inline step name inside
  `LOAD_CMD_NEW` (renamed `LOAD_CN_BSLK`); a second `SEAL CHAIN CHAIN_START` plus a
  duplicate `CHAIN_START` from a from-scratch reimplementation.
- **Lesson:** duplicates are never intentional here. Rename immediately. Do not
  spend tracing effort proving each one is "live".
- **Detect:** `check_duplicate_labels.py` (later `_duplicate_labels` in
  `iris_source.py`).

### Pattern 11: Unreachable Guard/Setup Instruction (label before the real entry point)

- **Symptom:** a guard (`JZ`, `CLEAR`) sits textually before `NOVA <name>`, but
  callers `RVOCA` to `<name>` and land past the guard. Auto-chain only governs
  what happens *after* a jump target.
- **Instances:** `LOAD_CU_CK0` before `NOVA LOAD_CMD_UNKNOWN` (the "unknown word
  -> skip line" guard for `===` dividers never ran; fixing it moved 6 -> 20 in the
  thirty-fifth round); `LOAD_DBL_RBASE`/`RLEN`/`RPOS` before
  `LOAD_DISPATCH_BUILT_LINE` (found one after another); `AL_SRCCK`
  (`ADD_LUMEN`); `LN_PVCK` (`LINK_NEXT`); one in the FOR override-skip logic.
- **Fix:** give the guard the real entry name, and rename the old first step so
  that it auto-chains after the guard. Callers do not change.
- **Lesson:** "the check exists in the source" is not evidence that it runs.
  Verify reachability on the real call path.
- **Detect:** `check_unreachable_entry.py`, which looks at RVOCA targets whose
  preceding same-block label is never referenced (RREDI-preceded cases excluded to
  avoid loop-label noise).

### Pattern 12: Shared/Intermediate Register Clobbered by a Called Subroutine

- **Symptom:** a caller relies on register R surviving an `RVOCA`, but the
  callee uses R as internal scratch.
- **Instances:** `SK_TMP` across `LOAD_RKVA_RESOLVE` (`LOAD_RKVR_DIGS` does
  `Sub ... Exit=SK_TMP`); `RA_HT_WORD` in `HT_INSERT` across `HT_NAME_STORE`
  (corrupted slot1 writes through address 77 and `RA_FLUX_BOTTOM`; this was first
  mistaken for a name-pool overflow). Then `htable.re` again, three more times:
  `RA_HT_LID`, `RA_HT_SLOT` and `RA_HT_PROBE` across `HT_NAME_CMP`. According to
  `htable.re`'s own comments, `RA_HT_PROBE` came back holding a name length, so
  `HT_IN_FULL` fired at about 19% occupancy. Also `RA_BS_TMP2` shared between
  `BS_PARSE_INT` and the hash loops (fixed with `RA_BS_PIVAL`); `RA_TMP`
  (volatile scratch per `registers.re`) used as a counter across `SCAN_ALL_LUX`
  in `LIMITER_CHECK` (fixed with `RA_LIM_COUNT`); `BS_TOKBUF_BASE` (a byte
  buffer) overwritten by a second token read, so `SPAN` copies its name out
  first.
- **Fix:** save into a dedicated `*_SV` register (or use a dedicated register)
  and restore after the call. Make sure the jump actually reaches the save (see
  the "jump bypasses a reset" pattern below).
- **Detect:** no reliable mechanical check. Heuristic scanners (write-before /
  read-after / callee-also-writes) were mostly false positives: documented return
  values (`VS_TEST_SET` -> `RA_FLAG`), in-place transforms (`BQ_ADDR`: RA_TMP in
  and out), and scan-window overrun. Trace one register's value across one call
  instead. `check_htable_reentrancy.py` covers the htable case.
- **Related, register-name mismatch:** `LOAD_READARG_KV` checked `SK_FLAG`, but
  `BS_SCAN_EQ` writes `RA_BS_FLAG`. It had only ever worked because dead code
  left a compatible value in `SK_FLAG`. Fixing it took builder-macro successes
  from 31 to 80 (eleventh round). Also not mechanically detectable.

### Pattern 13: Hash Table Comparing Only a Truncated Hash, Not the Full Name

- **Symptom:** a lookup returns an unrelated symbol.
- **Causes, in order found:**
  1. `lexer.re`/`wax.re` masked the running DJB2 hash to the 18-bit
     `BS_HT_MASK` after every byte (8 sites), so the "stored hash" used to
     separate slot collisions was the same narrow value.
     `PS_LX_SFUNC`/`PBKT_103` both hashed to 88681. Fix: keep the full hash, and
     mask only for the slot index. `BS_LOOKUP`/`BS_INTERN` mask to `MASK_LOW32`
     for the stored hash32, and `loader.py`'s `BS_HTAB` pre-population mirrors
     this. `BS_HT_MASK` itself is correct.
  2. Eight call sites (`LOAD_CMD_NEW`, `LOAD_CN_INTERN`, `LOAD_APPLY_BACKFILL`,
     `PS_LK_PBKT`, `PS_LK_LSYM`, `PS_INS`, `PS_LINE_KLK`) set `RA_HT_HASH`
     without `And MASK_LOW32`, which corrupted the packed `(hash32<<32)|lid`
     slot.
  3. Even correct 32-bit hashes collide at about 28k symbols (birthday bound).
     `JEQ` collided with a pending forward reference. Fix: entries are now two
     words (packed hash plus a pointer into `HT_NAME_POOL_BASE`/`HT_NAME_POOL_PTR`)
     with a full byte comparison (`HT_NAME_CMP`) on every hash match. Storing more
     hash bits was rejected because it only delays the problem.
- **Follow-on bugs from the full-name comparison:** every `HT_INSERT` site must
  set `RA_HT_NAME`/`RA_HT_NLEN` (bootstrap words needed `SK_BOOT_NAMEBUF`);
  `HT_LOOKUP`/`HT_INSERT` must *read* slot1's value, not pass its address (before
  that fix, "Move" was re-created 846 times); and the Pattern 12 register
  clobbers listed above.
- **Detect:** walk the table with the real algorithm. Do not use a tool with its
  own hash reimplementation (the old `diag.py --htable` used XOR instead of the
  ADD-based `hash*33+byte` and ignored the packed format).

### Pattern 14: Forward-Reference Names Silently Failing (`BS_LOOKUP` vs `LOAD_INTERN`)

- **Symptom:** a plain `BS_LOOKUP` on a name declared later returns 0 or stale
  data. Nothing backfills it, and the jump lands on a Python-side address
  (`LOAD_RKVA_EXIT` -> 4016).
- **Mechanism:** `LOAD_INTERN` records `(SK_INTERN_SLOT, hash)` in a backfill
  list on a miss. `LOAD_APPLY_BACKFILL` resolves the list at the Wave 0.5 -> Wave 1
  boundary and at the end of the run (176 real entries per run). It existed
  unused until Pattern Z.
- **Instances:** `LOAD_CMD_NEWREF` (the self-hosted `SEAL`), whose target was
  almost always declared later (`SEAL X X_START`). This affected every
  `SEAL`-registered macro (builder successes went from 3 to 9).
  `LOAD_RKVA_RESOLVE`'s symbol path, which is shared by every builder-macro
  argument. `LOAD_CMD_RVOCA_IMPL`'s `sub`.
- **Gotcha:** backfill writes to a single pre-set destination, so
  `SK_INTERN_SLOT` must be the **final** destination, not an intermediate that
  the caller copies afterwards (`SK_MA_RESULT`). The callers of `LOAD_RKVA_RESOLVE`
  were reordered to compute their destination first (with `Idem`). For RVOCA the
  destination is `addr(name)+SLOT_E2` (3), computed per call, not the shared
  `RA_BS_EL1`.
- **Detect:** `check_lookup_sites.py` lists every `BS_LOOKUP` for manual review.
  Behaviour on a miss needs judgement: skipping or allocating a placeholder is
  fine; proceeding with 0 is not.

### Pattern 15: Missing Self-Hosted Dispatch Handler for a Real Command (`SPAN`)

- **Symptom:** `LOAD_CMD_SET_OR_SRF` matched only `'E'` (SET) and `'A'` (SAVE)
  as the second character, so `SPAN` fell through to `LOAD_CMD_UNKNOWN` with no
  handler. `SPAN`'s word held 82 (`RA_FLUX_BOTTOM`) by coincidence. That
  explained `AC_G_HALT` halts and jumps to Python's `Left` address (thirty-fourth
  round).
- **Fix:** `LOAD_CMD_SPAN_IMPL` reads the name, copies it to `SK_SPAN_NAMEBUF`
  before reading the count, allocates with `ALLOC_LUCES`, and runs `HT_INSERT`.
  It does not create a self-ref, which matches `_alloc_raw_block`.
- **Lesson:** "can't be self-hosted" is never true. Every command a `.re` file
  can contain needs a self-hosted path. (One round wrongly decided that
  SPAN/ALIAS/OWN/FREE were load-time only. The next round retracted it.) A rising
  file count does not show that each command was processed. Inspect the command
  word's compiled value.

### Pattern 16: Multi-Line Single-`/` Comments Need a `/` on Every Line

- **Rule:** `comments.re` is canonical. A line whose first non-space character is
  a single `/` is a one-line comment. There is no continuation by indentation.
  `//...//` is a toggled block.
- **Failure:** continuation lines written without `/` are read as code. A stray
  trailing `//` *opens* a block that swallows real code up to the next `//`.
- **Instances:** about 37 unclosed `//remark` comments in `yaku.re`, one of which
  swallowed its own LLVM template table; `aria.re:129`, which swallowed `NEW
  LOAD_START`; two in `htable.re` (the `HT_NAME_POOL` header and a `SPAN
  HT_NAME_POOL_BASE` trailing comment), which swallowed
  `HT_NAME_STORE`...`HT_CLEAR` ("96 command lines swallowed"; fixing them moved
  `SK_FIDX` from 0 to 39 in the thirty-third round); early on, more than 1000
  comment lines without a `/` prefix; `comments.re` `CHAIN` lines with a stray
  `NOVA ` prefix.
- **Detect:** `check_comments.py` (a faithful copy of `_strip_comments`). Run it
  after editing any multi-line comment. Formerly also `check_block_comments.py`
  and `classify_spans.py`, which flagged spans whose swallowed text starts with a
  command keyword. `aria.re:139` was a false positive, confirmed by exact
  simulation of `BS_BLOCK_SKIP`'s toggling. Prose that starts with a command-like
  word: quote the word (`"Set" via Move`), see CONVENTIONS.md.

### Pattern 17: Arena Sizing Must Track Newly-Working Allocators

- `AETHER_SIZE` went 2M -> 4M (full-name table plus the 1M name pool) -> 8M
  (`SPAN` finally allocating `intern.re`'s two 131072-word tables; at the halt,
  `RA_ALLOC_TMP` was 4,083,251 against `RA_FLUX_BOTTOM` 4,063,232). The step
  budget (`max_steps`) grew in the same way, from 100M to 200M.
- **Lesson:** allocators that used to fail silently start consuming real space
  once they are fixed. Before raising a ceiling, confirm that the limit is really
  exhausted, not corrupted: compare the halting allocator's cursor with
  `RA_FLUX_BOTTOM`. Raise budgets deliberately. Never raise one to hide a loop
  (see the loop-detection lesson).

### Pattern 18: A Comment-Only Line Inside a Macro Body Closes the Whole Block

- **Symptom:** a literal `{N}`/`{X}` is left in compiled symbol names
  (`LOAD_MAIN_WBM{N}`/`LOAD_MAIN_WBH{N}`).
- **Cause:** a `loader.py` bug. `_read_re_file` treated a comment-only line
  (empty after `_strip_comments`) the same as a truly blank line, and a blank
  line closes every open block. A comment inside a `FOR` body ended the `FOR`
  context. The long-trusted Python side had always had this bug; the self-hosted
  `FOR` exposed it.
- **Fix:** check the original, unstripped line before closing contexts. It was
  reproduced first with a two-line test file.
- **Detect:** scan the frozen symbol table for `{` or `}` in any name. This is an
  "is the output complete" check, not just "did it finish".

### Pattern 19: Two Functions With the Same Contract, One Correct, One Not — Fix Found in One Doesn't Get Applied to the Other

- `LOAD_DISPATCH_LINE` reads a fresh token from the file.
  `LOAD_DISPATCH_BUILT_LINE` dispatches content that is already in
  `BS_TOKBUF_BASE`. Calling the former for built content silently discards the
  content. This was fixed once for FOR (`LOAD_READ_BODY`/`LOAD_EXPAND_TEMPLATE`
  scaffolding) with a warning comment. It was then found again in
  `LOAD_PROCESS_BODY` (SAVE bodies), which also never copied line content (the
  content now goes to `LD_NI_OUT`). That bug was latent because every real SAVE
  is one line.
- **Lesson:** a warning comment protects only the place where it is written.
  After fixing an "A vs B" confusion, grep for every caller of the wrong one
  (`grep -n 'RVOCA.*LOAD_DISPATCH_LINE'`). Now only `LOAD_FILE` legitimately
  uses it. Track every "I'll come back to this" note. This one nearly got lost.

### Pattern N: FOR macro expansion bug (fixed) → root cause of 27 op=0 issues

- **Status: fixed** in `loader.py` `_expand_indent`. `FOR` was not in
  `_INDENT_LEADERS`, so it fell into `mode='prepend'`, which prepended the first
  list element to every body line. `CLEAR LOAD_MAS_CLR{N} {X}` became `CLEAR
  RA_MA0 ...`, which re-allocated `RA_MA0` as a NOVA (842 -> 601114) after
  `macros.re`'s `_SPN` macros had already baked in 842. `RA_MC_LUX` was then
  always 0, and every builder `Write` went to address 0.
- **Fix:** `mode='for'` emits one copy of each body line per element, with `{N}`
  replaced by the index and `{X}` by the element.
- **Why code cites Pattern N for RCALL_AT/EMIT being unwired:** after the fix,
  27 of 170 `op=0` issues went away. The other 143 are builder-class macros
  (`LH, LR, LT, WALK_NOVA, RCALL_AT, EMIT, EMITI, PUTBYTE`, ...) that only
  LOAD_MAIN wires. See Pattern R. Calling `LH_N0_W0` by hand showed the chain
  works end to end once it is reached.

### Pattern N+1: PB_FLUSH reads from RA_OB_BASE but PUT_BYTE writes starting at RA_OB_BASE+1 (off-by-one, output is always wrong/zero for first lux)

- **Status: fixed** (`output.re`). `PB_ZERO_LUX` advanced `RA_OB_ADDR` before
  writing. Now the advance happens only in `PB_ADV_LUX`, after 8 bytes (when the
  shift reaches 64). Invariant: `RA_OB_ADDR` always points at the lux currently
  being written, starting at `OB_BASE`. Also, `EMIT_PACKED_STR` used stride +2
  (`DATA_LUX_MIN`), but packed strings use stride 1, matching `_pack_string` and
  `LOAD_PS_EMIT_BYTE`. Verified with "Hello, World!\n" through
  `EMIT_STR_ENTRY` + `FLUSH`. REFERENCE.md's "treat output macros as
  non-functional" note predates this fix. The remaining reason is Pattern R.

### Pattern O: LIMITER_ADOPTED scans all Lux (O(N) at exit) — FIXED

- `LIMITER_ADOPTED` (`limiter.re`, called once from `EXHALE_REPORT` in
  `wane.re`) called `SCAN_ALL_LUX` to find any `ACCORD_USE -> LIMITER_ACCORD`.
  Now `loader.py` sets a `K_LIMITER_ADOPTED` flag at freeze time and the check is
  O(1). A runtime hook for `LINK X ACCORD_USE LIMITER_ACCORD` added after freeze
  would be needed if adoption can happen dynamically.

### Pattern P: LINK command — different lumen storage model in Python vs Reca

- **Status: closed, not a bug** ("Pattern P update", 2026-06-23). It was once
  thought that Python embeds lumina in the source lux while Reca allocates
  separate 2-luce blocks. Checked: both use the embedded model. `alloc.re`: "There
  are NO separate lumen blocks." `_alloc_data` sizes the lux as `1 + 2*N + 1`
  from the lumen prepass. The misreading came from outdated documentation.

### Pattern Q: Yaku Voca/Redi templates missing call-stack operations

- **Status: open (last recorded).** Yaku's LLVM IR templates for `Voca` emit
  `store RA_LINK=NXT_N; br`, with no push (`RA_SP -= FRAME_SIZE;
  aether[RA_SP] = old RA_LINK`). `Redi` emits `load RA_LINK; br`, with no pop.
  Compiled code with call depth greater than 1 will return incorrectly.
- **Fix needed:** `RA_SP_ID` and `RA_FRAME_SIZE_ID` escape bytes in the template
  substitution, then push and pop with inline GEPs. See the LATENT ISSUE comment
  in `yaku.re`.

### Pattern R: Python's Wave-B only natively expands ~16 primitive commands — everything else is unbuilt content until LOAD_MAIN runs it

- **Status: architectural limit of the bootstrap scaffold, not a bug.**
  `_wave2_line` natively handles only `NEW, NEWSET, NOLINK, SET, NEWREF/SEAL,
  SETREF, LINK, NOVA, BLOCK, RVOCA, RREDI, CLEAR, NOP, JEQ, JZ, ALLOC_TO`.
  `SWITCH/FOR/SAVE/NONOVA/CHAIN` are expanded earlier as text.
  `LR, LT, RCALL_AT, EMIT, EMITI, PUTBYTE, WALK_ONE, LINK_OP, WALK_NOVA, UNLINK_OP,
  LX, LH` and any similar custom macro are expanded **only** by LOAD_MAIN at run
  time. In the Python build their luces are correctly sized (Pattern S) but their
  contents are never written.
- **Consequences:** most of `yaku.re`'s internals are unbuilt, which is why
  `P0_NID` emits 0 bytes. To guarantee a Python-built entry point, use a native
  primitive (usually `NOP name`), not `LR`/`EMIT`. See the `RTB_LUX_BODY` comment
  in `yaku.re`. Do not reimplement these macros in Python. Self-hosting progress
  is the real fix.
- **Baseline:** `diag.py --lint` (2026-06-20) found 144 phantom NOVAs and 19 dead
  references, all built by these macros (`ring.re`'s `RI_LINK`/`RING_PUSH`/
  `RP_GR_*`, `yaku.re`'s `EM_EPRELEQ_J`/`EB_*`/`PA_ISNEW_J`/`PGB_*`, `RTB_FTK_J`).
  `FOR_V2_DESIGN.md` quotes a later baseline of 163. Compare against the baseline
  after changes.
- **Self-hosted side:** the word bootstrap into `SK_HTAB` must cover this whole
  list. `RVOCA/RREDI/JZ/ALLOC_TO` were found missing in the twelfth round.

### Pattern S: `_scan_nova_names`'s NOVA-target sizing prepass was a hand-maintained list — now derived automatically from macros.re

- **Status: fixed.** Origin: Wave-1's generic "Unknown command" stub gave
  future NOVA targets a 2-slot layout instead of `NOVA_SIZE=7`, and `_define`'s
  "already exists" guard blocked any later resize. `_scan_nova_names` scans every
  file first, and `_alloc_data` consults `_nova_names`. Its keyword list was
  hand-maintained (`RVOCA, RREDI, NOP, NOVA, JZ, CLEAR, ALLOC_TO, JEQ`) and missed
  12 macros, which broke `RTB_LUX_BODY`. `_discover_nova_naming_commands` now
  derives the set from macros whose body has
  `Move El1=RA_MA0 ... Exit=RA_MC_LUX`. `NOVA` stays hardcoded. `ALLOC_TO` keeps
  its `_J`/`_K` suffixes.
- **Later:** discovery also had to see Python-side naming builtins (`HASH_STEP`).
  `_nova_lux` had been re-allocating stubs and
  repointing symbols 58 times.
- **Detect:** `check_nova_sizing.py` (0 violations across 3784 names at the
  time).

### Pattern T: Deferred items from the 2026-06-20 cleanup session

1. **Open, low priority:** `SAVE_EMIT_SAVES`/`SAVE_EMIT_RESTORES` do not reject
   `RA_LINK` the way the Python `SAVE` preprocessor does. They are unrolled shifts
   over `RA_MA1..4`, so an early exit would drop the later registers. The fix
   needs a filter-and-reshuffle at each position.
2. Closed: `_wave1_line`'s stub re-allocation check is not needed in the generic
   fallback, thanks to Pattern S.
3. Dropped: a meta-question about why LOAD_MAIN stalled.
4. Closed (fourteenth round): `test_macros.py`, `repl.py` and `interpreter.py`
   have no hand-maintained command lists.
5. **Macro-duplication audit.** `ALLOC_TO` ("constant count -> `ALLOC_LUCES` ->
   one unconditional Move of the result"): 7 converted (`PS_NW_ALLOC`,
   `PS_BLK_ALLOC1`, `PS_MOF_NEW`, `PS_DEF_STR`, `RING_INIT`, `RP_GR_NEW`,
   `LOAD_CNI_ALLOC`), plus `SWITCH_E1_AC4`/`AC5`. The sweep is exhaustive (all 16
   remaining sites differ: computed count, `Write` before the save, conditional
   save, multiple destinations, or a foundational allocator). **Still not audited**
   for manual duplication: `WRITE_NOVA_SLOTS`, `LR`/`LT`,
   `EMIT`/`EMITI`/`PUTBYTE`, `WALK_ONE`/`WALK_NOVA`, `LINK_OP`/`UNLINK_OP`.
   Another duplicate of this kind: `AUTOLINK` re-implemented `LINK_NEXT`'s
   adjacency formula inline, even though `LINK_NEXT`'s comment said it was shared.
   It now calls `LINK_NEXT`.

### Pattern U: NEWSET integer values wrong in runtime LOAD_MAIN (TAB=18, LF=20)

- **Symptom:** `aether[TAB]=18`, `aether[LF]=20`.
- **Cause:** the key-value integer resolver (`LOAD_RKVR_INT`) computes
  `addr(C_0) + 2*N`. It had used `C_0`'s value instead of its address (fixed
  with `RA_C0_REF`), and it shared `RA_BS_TMP2` with the hash loops (fixed with
  `RA_BS_PIVAL`, 5 call sites).
- **Still open (dormant):** the resolver returns the *address* of `C_N`, and
  `NEWSET_START` writes that address. Adding a `Read` breaks large values,
  because the `C_N` constants are sparse (only `C_0..C_19` are contiguous). The
  real fix is to change the resolver contract to "look up or lazily allocate a
  lux holding N", as Python's `_resolve` does, together with a matching Python
  pre-allocation (ROADMAP D1). The fourteenth round found **zero** real call
  sites that pass a bare integer through this resolver, so there is no current
  impact.

### Pattern V: PS_MAIN runtime parser reads 0 lines from file

- **Status (last recorded): root cause known, blocked on Pattern U.** `PS_RL`'s
  `JEQ PS_RL_NL PR_BYTE LF PS_RL_DONE` never fires while `LF` holds 20, so the
  whole file becomes one line. `PS_MAIN` (`parser.re`) is the pure-Reca runtime
  parser, not on the LOAD_MAIN path. Not re-verified since `NEWSET` was renamed
  to `SET`.

### Pattern W: Registers (`RA_*`) have no per-thread ownership — will break under real parallelism

- **Status: design agreed, partly implemented.** `loader.py` now treats `OWN` as
  a placement (colocation) concern and marks every `NEW RA_*` register `OWN`
  automatically. Per `loader.py`'s comments, all 239 already carried it by hand.
  Whether real per-thread copies exist is not recorded here. Check `loader.py`
  and PLAN_SCHEDULER. This blocks PLAN_SCHEDULER steps 2-3 (a real second thread
  for `SCHED_FORK`).
- **Problem:** about 251 `RA_*` scratch registers (addresses 74..5645) are single
  shared aether slots. Two threads would clobber each other. This is the same
  class as the symbol-table sharing fixed with `SK_HTAB_000`.
- **Rejected:** a per-thread name->address translator (addresses are baked into
  every instruction at build time); double indirection for every register
  (changes the meaning of almost every instruction); per-thread copies of only
  the scheduler's own registers (the race involves all scratch registers).
- **Agreed direction:** collect the registers into one contiguous block. Each
  thread gets a cheap copy of that block, allocated like its stack by a real
  region primitive (see PLAN_ARENA_UNIFY.md). Instruction addresses do not
  change. The rest of the aether stays shared.
- **Resolved: default stays shared, personal is the explicit opt-in.** `NEW`
  keeps meaning one shared slot. A forgotten opt-in then stays visible and
  predictable instead of becoming silently private. This follows GLOSSARY's "only
  Aether" model. The marker word is `OWN` (the author confirmed it; `PUBLIC` was
  rejected and `SHARE` considered). The opposite is `FREE`. Both work standalone
  (`OWN RA_TMP`, which marks an existing name) and as a `NEW` tail modifier
  (`NEW RA_TMP | OWN`). They only change which block the loader collects the name
  into. They never change lux contents.
- **Bug found along the way:** `_new_modifiers` was used through `getattr`
  without being in `Loader.__slots__`, so it would have raised the first time
  ` | ` appeared. Fixed.
- **Undecided:** the general "new region of size N now" primitive (shared with
  PLAN_ARENA_UNIFY). Register collection as a `.re` reorganisation or as a
  loader relayout (the loader relayout was preferred, but not confirmed).

### Pattern X: `macros.re`'s `NEWREF`/`SETREF` runtime macro registrations are dead code (found, not removed)

- **Status (last recorded): found, deliberately not removed.** After the `REF`
  -> `SEAL` rename, `SEAL NEWREF NEWREF_START` and `SEAL SETREF SETREF_START`
  (plus handler labels `NEWREF_CKJ/CROSS/CRW/RET/SELF/SRET2`, `SETREF_START`) could
  not be reached: no `.re` file uses `NEWREF`/`SETREF` as a command. `NEWSET`'s
  registration was live at the time. Since then `macros.re` has been merged into
  `commands.re` and `SEAL` retired. Re-check before acting.
- **Also cited as Pattern X** (by `iris_common.py`, TOOLING.md, DECISIONS.md):
  the lesson that **two independent copies of a registration scan, or a
  hand-maintained table of name->entry, drift out of sync**. The concrete case is
  `diag.py`'s hand-written macro->entry table, which assumed `JEQ_START`/`JZ_START`
  (the real entries are `JEQ_N1`/`JZ_N1`) and never checked `NEXO` at all.
  Replacing it with `discover_seal_registrations()`, which derives the table from
  the source, exposed Pattern Y. The rule is to derive such tables from the
  source. Today that is `iris_common.wax_cmd_registrations()` over `WAX_CMD`
  lines.

### Pattern Y: `NEXO` name collision — two unrelated things both claim the same SEAL registration

- **Status: fixed.** `macros.re:825` `SEAL NEXO NEXO_START` (the relation macro,
  36 uses in `yaku.re`) collided with `parser.re:1623` `SEAL NEXO PS_LINE_WAX`
  (PS_MAIN line dispatch). The later file in glob order won in freeze's
  post-LOAD_MAIN restore loop, so `NEXO.word` pointed at `PS_LINE_WAX`. The
  `parser.re` registration was removed, and the macro-wiring check now confirms
  `NEXO` -> `NEXO_START`.
- **Older instance of the same class** (the historical section "O. PS_MAIN
  dispatch: NOLINK and YAKU_NEXO use wrong handlers"): `parser.re`'s `NEWREF
  NOLINK PS_LINE_NOLINK` / `NEWREF YAKU_NEXO PS_LINE_WAX` were silently ignored
  because `macros.re` loads first. Closed in the thirteenth round: `NOLINK` has
  one registration and `YAKU_NEXO` became `NEXO`, which is this pattern. The other
  `parser.re` `SEAL X PS_LINE_*` entries (`NEW/SET/LINK/LOAD`) were checked and
  are uncontested, because first-byte fast paths never read those words.
- **Lesson:** registering the same name from two files resolves silently by file
  order. Give the parser's dispatch table distinct names or its own table.

### Pattern Z: LOAD_MAIN's symbol table has no declare-before-use barrier across files — real names never resolve

The central self-hosting investigation (2026-07-05 onwards, 45 "rounds"). Code
comments cite it for many things. This entry collects the distinct findings. The
narration has been dropped, and round tags are kept so references still resolve.
Most sub-findings were catalogued as Patterns 9-19. The additional findings are
below.

**Original finding: the barrier.** Python is strictly phased (Wave-A, A2, B0, B
over every file), so any declaration is visible before any use.
`LOAD_MAIN_HTSW` deliberately switches `BS_HT_BASE` to an empty `SK_HTAB_000`,
which is correct: self-hosting must build its own table. But LOAD_MAIN's Wave 1
builds each file in a single pass, and files interleave (on purpose, as
groundwork for the scheduler; see PLAN_SCHEDULER). A use could therefore precede
its declaration in another file. At the time, 96.5% of the 6410 writes to
`SK_CMD_ADDR` were 0, including `NOVA`, `RREDI`, `NEW`.
- **Fix: Wave 0.5 declare-only pass.** `SK_DECLARE_ONLY` is set, and every file
  goes through the unchanged `LOAD_FILE`. `LOAD_CMD_UNKNOWN` skips non-declaring
  commands. The declaring set must be exactly Python's `_BOOTSTRAP_CMDS` (`NEW,
  SET, LINK, NEWSET, ALIAS, NOVA, BLOCK, SPAN, NOLINK, SEAL, OWN, FREE`), not a
  guessed subset. Later, `CLEAR/NOP/RVOCA/RREDI/JEQ/JZ/ALLOC_TO` joined the
  allowlist. The Wave 0 -> Wave 1 transition is where a real scheduler will later
  fork per file and join.
- Needed along with it: `LOAD_CMD_NEW`'s "already declared" early return must
  `BS_SKIP_TO_EOL` first. Re-declaration is now the common case, and a `| OWN`
  tail would otherwise be dispatched as a command.

**Command words must exist in LOAD_MAIN's own table.** After the switch, even
`NEWSET/CLEAR/NOP/ALIAS/SPAN/OWN/FREE` (and later
`RVOCA/RREDI/JZ/ALLOC_TO`) were missing from `SK_HTAB`, so the allowlist could
never match. Fix (ninth and twelfth rounds): a word-bootstrap block after the
switch. It computes each word's DJB2 hash with `FOR` over its characters (no
pasted magic numbers; the author objected to them), captures addresses with
`_REF`/`Idem` (Pattern 9), and runs a uniform `HT_INSERT` loop. Builder-macro
successes went from 9 to 31. In `freeze()`, `loader.py` also pre-allocates the
full `_BOOTSTRAP_CMDS` set (twenty-seventh and twenty-eighth rounds). It had been
narrowed to 5 words because of a 6/42 reading that did not reproduce, and the
earlier "SEAL_ANCHOR SEAL" hack was replaced by explicit pre-allocation.

**SEAL needed a two-phase self-hosted implementation** (seventh round).
`LOAD_CMD_NEWREF` resolved targets immediately. Python solves the same problem
with separate allocate and wire phases. Fixed with `LOAD_INTERN` plus backfill
(Pattern 14). `SEAL`'s registration to `LOAD_CMD_NEWREF` was first reverted
because of an apparent 42 -> 19 regression. That regression was a diagnostic
running with `max_steps=200` (`HT_CLEAR` alone needs about 460k steps). The
registration was correct. `LOAD_CN_INTERN` also always allocated 2 luces,
ignoring `lcount`; it is now sized like `LOAD_CMD_NEW`
(`1 + 2*lcount + (lcount>0)`).

**SEAL skips the generic argument pre-read** (sixteenth round). `SEAL` reads its
own tokens, while `LOAD_READARG_KV` pre-reads `MA0..MA7` for builder macros. Fix:
an explicit `JEQ SK_CMD_ADDR SEAL` exception in `LOAD_CMD_UNKNOWN`, following
`PS_LINE`'s design (no shared pre-read; each handler reads what it needs). Do not
move the pre-read into `JEQ/JZ/WALK_ONE/LH/LX`. They are dual-purpose: they are
top-level commands and are also `RVOCA`'d internally with MA preset (for example
`SWITCH_START`'s `RVOCA SWITCH_E1_JEQ JEQ`). The dead "native handler" branch
(target Exit == `RA_LINK_REF`, never matched by any of the 46 registrations) was
removed in the eleventh round, together with its `LOAD_CU_BUILDER`/`LOAD_CU_CALL`
labels. See "LOAD_CU_BUILDER dispatch path is dead code" below.

**Text-template constructs have no runtime command** (sixth round). `CHAIN`,
`FOR`, `NONOVA` and `SAVE` are expanded by Python's `_read_re_file`/
`_expand_indent` before the waves run. LOAD_MAIN reads raw bytes. A literal
`CHAIN X` was interned fresh as a self-ref and `Voca`'d into nothing. Earlier
"moving crash point" findings were real bugs that happened to come before the
first `CHAIN`. Status: `CHAIN_START` and `SWITCH_START` already existed. The
lesson is to grep for `X_START` and the registration line before writing
anything: builder-macro bodies are named `X_START`, while `LOAD_CMD_X` is only
for native primitives. `FOR` got `LOAD_CMD_FOR_IMPL` (thirty-eighth and
thirty-ninth rounds): header parsing into an IRIS name pool, `FOR`/`FREE`
distinguished by the second character, override tables with the three-tier
priority from FOR_V2_DESIGN.md, and key=value pairs. That work needed
never-declared `RA_FOR_IRIS_BUF`/`RA_FOR_IRIS_IDX_BASE` to be declared, and it
fixed Pattern 19 and Pattern 18. It also hit a `JEQ` used where `ULess` was
needed (overshoot by one, infinite loop).

**Tracing-tool reliability** (fifth to tenth rounds). Several "findings" were
tool artifacts: counts of "16 SEAL calls", "scattered hashtable failures" in
`derived.re` (from correlating hash values of common op names such as `Move` and
`Load` with events), "off by NOVA_SIZE" words (from re-running LOAD_MAIN on a
frozen snapshot), and the "0 vs 16" contradiction. See "Debugging and tooling
lessons".

**The htable saga** (forty-third round). `check_graph_equivalence.py` found
about 1900 self-hosted op-primitives looking like garbage. Four bugs in
`htable.re` and `intern.re`:
1. `BS_INTERN` never wrote the self-ref promised in its header. `ALLOC_TO`/
   `ALLOC_LUCES` never initialise content. Added
   `Write El1=RA_BS_RESULT El2=RA_BS_RESULT`.
2. `HT_LOOKUP`/`HT_INSERT` passed slot1's address, not its value, to
   `HT_NAME_CMP`, so every hash match looked like a different name ("Move"
   created 846 times).
3. `RA_HT_LID` was clobbered by `HT_NAME_CMP` once the upsert path first became
   reachable (`RA_HT_LID_SV`).
4. `RA_HT_SLOT` was shared between the outer probe index and `HT_NAME_CMP`'s
   character counter. This was a real **infinite loop**: the probe cycled among a
   few buckets. It was confirmed with `trace.py` `sample_every` (the same
   4-instruction cycle across 13 samples, 5M steps apart). Fixed with
   `RA_HT_SLOT_SV`.
Also in the forty-third round: `trace.py --find-writer` had lost its `def` line
(restored); `SK_SPAN_NAMEBUF` needed `Idem`; `LIMITER_CHECK`'s `RA_TMP` was
fixed with `RA_LIM_COUNT`.

**forty-fourth round.** Wave 1 then stalled at 1/42. "Just slow" was assumed
and was wrong. Full-run counts showed `RA_LOAD_RPOS` frozen for 190M of 200M
steps. Two bugs:
- The new save instructions used `El2=` as the destination. `Move` writes only
  to `exit`. Five instances, two older than the session. A project-wide sweep of
  every Exit-writing op found no others.
- The hash-match `JEQ HT_LK_HM ... HT_LK_NWADDR` still targeted its old label and
  skipped every inserted save. It was retargeted to `HT_LK_PROBESV`/
  `HT_IN_PROBESV`. Result: 14/42 within 200M, 20/42 at 500M (the budget was
  reverted, deliberately).

**forty-fifth round.** A fast hotspot profiler (`fast_profile.py`) plus
resumable checkpoints (`long_run.py`) showed `HT_NAME_CMP`'s character loop
using 80-90% of steps, with about 3000 comparisons per call on names of 30
characters or fewer. `JEQ HT_NC_LENOK ... HT_NC_BYTES` skipped `HT_NC_I` (`CLEAR
RA_HT_SLOT`), so the character index started from the outer probe index.
Retargeted to `HT_NC_I`. `SK_FIDX` reached 42 within 200M. This was later shown
not to mean a correct build (see Current status). 681 graph-equivalence
mismatches remained (open issue).

**Earlier SK_FIDX-count milestones, for reference only:** the SPAN handler
(thirty-fourth round) and the 4M->8M arena increase (thirty-seventh round) are
cited from `aether.py`. The arena growth is described under Pattern 17.

---

## Other recurring patterns (unnumbered in the original log)

### Implicit fall-through across a `NOLINK` boundary

- **Symptom:** execution silently continues at `pc + NOVA_SIZE` (the
  physically next allocated instruction), which is often inside an unrelated
  handler. `next == 0` means **fall through**, not halt (`_run`: `if raw_next ==
  0: next = pc + NOVA_SIZE`).
- **Cause:** a `NOLINK` correctly protects a block from unrelated auto-chaining,
  so the *intended* predecessor also gets no link. Any `JZ`/`JEQ`/`CLEAR`/call
  immediately before a `NOLINK` needs an explicit `Jump`.
- **Instances:** `LOAD_CMD_BLOCK` (`JZ LOAD_CB_EXCZ` followed by `NOLINK`; its
  fall-through ran into `LOAD_CMD_NEWREF` internals and wrote through a zero
  pointer, corrupting **aether address 0**. Fixed with
  `NOVA LOAD_CB_EXFALL Jump Exit=LOAD_CB_DONEW`); Wave 0.5 -> Wave 1 (looped,
  reported "1282/42 files, 3052%"); `LOAD_MAIN_HTPT` -> SEAL registration block ->
  Wave 0; `FREE_FLUX_ZONE`'s return (fixed pre-emptively). Code comments count 12
  instances of this "fallthrough-hijack" class in one session, 8 of them a
  `NOLINK` right after an `RVOCA`/`RREDI`.
- **Detect:** `iris` invariants 12 (precise fallthrough-hijack) and 13 (`NOLINK`
  right after `RVOCA`/`RREDI`). Or read the compiled next field,
  `int(aether[addr + 5])`, and compare it with the intended target.

### Jump bypasses a reset/save instruction sitting between it and its target

- **Symptom:** no crash and no lint finding. The run is just slow, or eventually
  hangs.
- **Cause:** a `JEQ`/`Jump` targets a label past a `CLEAR` or save instruction
  that its path needed. This often happens after instructions are inserted
  before an existing jump target without retargeting the jump.
- **Instances:** `HT_LK_HM` skipping `HT_LK_PROBESV` (forty-fourth round);
  `HT_NC_LENOK` skipping `HT_NC_I` (forty-fifth round).
- **Detect:** only a crude regex sweep so far (JEQ target is not the next line,
  and a `CLEAR` lies in the skipped span). It found 19 candidates in `htable.re`,
  `lexer.re`, `macros.re`, `parser.re`, `wax.re` and `scheduler.re`. Only a few
  were checked (open issue).

### Loops with no real termination at EOF or end of input

- `LOAD_EXPAND_TEMPLATE`: the exit test `SK_ET_PTR == SK_BODY_PTR` stays true
  forever, so it re-flushed endlessly, and its `RREDI LOAD_ET_DRET` was
  unreachable. Fixed with an `SK_ET_DONE` flag: flush once, then return.
- `SWITCH_LINE` treated every empty token as a blank line to retry, even at real
  EOF (`RA_LOAD_BYTE==0`). It burned the whole 100M budget (the run returned
  exactly 100000001 steps). Fixed with `SWITCH_T1_EOFCK`.
- `BS_SKIP_TO_EOL` always scanned to the *next* LF, even when it was already on
  one, and swallowed the following line (the original "stuck at aria.re, 4/42"
  bug through `LOAD_PP_SKIP`). Fixed once in the shared primitive (a no-op when
  already at LF or EOF), which repaired about 20 call sites. The local guard was
  then removed.
- **Lesson:** "do at most one final action", and fix the shared primitive, not
  the call site.

### Self-hosted construct diverges from Python's semantics

Mirror `loader.py` exactly ("make it work like Python, but in Reca").
- `SWITCH`: Python rechecks indentation on every line. The self-hosted version
  checked only once (`SWITCH_RDLINE`), so all 5 loop-backs now go through
  `SWITCH_RDLINE`. Python rule: **no arrow means exactly 2 tokens, value then
  destination; the arrow is required for 3 or more values** (the author
  confirmed). The self-hosted version never handled the no-arrow case
  (`SWITCH_T2_NOARROW` was added). This corrupted `yaku.re`'s `ETH_*` `SWITCH`
  and clobbered `output.re`'s `EMIT_PACKED_STR`.
- Unifying `LOAD_READ_BODY`, `NEXO`'s loop and `SWITCH`'s loop into one
  higher-order primitive was considered and **rejected**. There are only 3 call
  sites, and a `Voca` callback costs more than it saves. Revisit if a fourth
  appears.
- `LOAD_RL_IMPL` consumed the indent-peek byte without un-reading it, which
  dropped the first byte of each terminating line. It now does `RA_LOAD_RPOS -= 1`
  on non-EOF returns.
- `_resolve()` turned bare numeric literals into raw addresses. Address 0 also
  reads as "no operand" in truthiness-gated ops. It now maps a literal to a
  `C_<n>` register (`_lit_const_cache`), generalising `JZ`'s `RA_C0_REF`
  convention. This fixed the `BS_READ_TOKEN` EOF infinite loop. `SWITCH`'s inline
  form is pre-expanded to `JEQ` by `_expand_indent`, so a Reca-level fix
  (`SWITCH_RESOLVE_VALUE`) was secondary.
- When the self-hosted design strains, check whether `parser.re`/`PS_MAIN` (a
  second, independent implementation at the runtime-parsing layer) has already
  solved the same shape of problem before inventing syntax. The `SEAL`
  pre-read exception came from `PS_LINE`.

---

## Open issues and future work

1. **Self-hosting is not achieved.** The file counter now reaches 82/82, but
   that is only a counter (see "Current status"). The real measure is
   `iris.py --graph-equivalence`: most named NOVA nodes are reproduced, but not
   all -- wrong nodes (mostly the Next slot), husks, and names missing from
   SK_HTAB remain. Measured earlier as *not* the cause of the old early stop:
   memory corruption (`RA_FLUX_BOTTOM` intact), structural faults (lint clean),
   and the `AC_G_HALT` exit (every build ends there).
2. **Graph equivalence: 681 structural mismatches.** They are concentrated in
   `macros.re` internals (`WISR_START`, `AUTOLINK_START`, `SWITCH_START`,
   `FOR_START`, `RCN_S0`, `EMIT_SPN`, `NOVA_START`, `BLOCK_START`, `NOLINK_SV`).
   Every `.Exit` points at "the next declared name". The question is unsettled:
   several comments claim `macros.re` is excluded from self-hosted
   reprocessing, but `loader.py`'s file list includes it. The self-hosted copies
   are probably inert (runtime `Voca`s resolve through the Python-built graph),
   but this is not verified.
3. **Fixed-point self-compilation** (LOAD_MAIN output equivalent to Python's)
   has never been checked. When building the comparison tool, resolve names
   through each graph's own symbol table (`loader.symbols` vs `SK_HTAB`) and
   compare relational shape, never raw addresses (Python's `Add` is at 8,
   LOAD_MAIN's was at 221209).
4. **Python-baked tables that LOAD_MAIN only reads** (`SK_DECL_TABLE`,
   `SK_OPENER_TABLE`, `SK_REGION_TABLE`/`SK_COMBO_SIZE`) must eventually be
   derived by Reca itself. ROADMAP says not to do this now.
5. **Double full-table probe.** A single lookup can walk the whole table twice
   (`RA_HT_PROBE` reaches 131072 = 2 x `RA_HT_SIZE`). Checking the numbers shows
   this is too small to explain the slowdowns, and it is not the `HT_NC_I` bug.
   Captured token buffers held non-ASCII garbage. Tools: `long_run.py`,
   `fast_profile.py`, `make_patched_run(on_write_dest=RA_HT_PROBE)`.
6. **Garbage tokens being interned** (2026-06-30, not explicitly closed):
   `IndexError` in `_read` inside `ALLOC_LUCES` (`AC_WM_CUR`). The e1/exit slots
   (should be 88 `K_WATERMARK` and 106 `RA_ALLOC_TMP2`) were overwritten with
   614519. `LOAD_CMD_NOVA`'s find-or-create (`LOAD_CI_NINT BS_INTERN` ->
   `LOAD_CI_EWGET` -> `LOAD_CI_OPTOK_NEW`) interned a non-text token
   (`'^\x00\x00\x00^...'`), found an existing NOVA (6061), and overwrote it. The
   file-position registers pointed at clean text in `accord.re`. A write-watch on
   `BS_TOKBUF_BASE` saw zero writes, possibly because it watched only `Write` and
   not `Move` with a computed Exit. This was likely superseded by the Pattern
   13/Z hash-table fixes, but that is unverified. The non-ASCII buffer content
   seen in item 5 has the same shape.
7. **Jump-bypasses-reset sweep:** 17 of 19 candidates are unchecked. It needs a
   checker that understands `NOLINK`-delimited basic blocks, not line distance.
8. **Pattern Q** (Yaku `Voca`/`Redi` templates have no stack push/pop) is open.
9. **Pattern U** (integer resolver contract) is dormant with no current call
   sites. **Pattern V** (PS_MAIN reads 0 lines) is blocked on it.
10. **Pattern W:** the register ownership block is not finished (see there).
11. **Pattern X:** dead `NEWREF`/`SETREF` registrations. Re-check whether they
    survived the `macros.re` -> `commands.re` merge.
12. **Pattern 3 and Pattern 5:** workarounds only. Root causes were never found.
13. **Pattern T:** `SAVE_EMIT_SAVES` does not reject `RA_LINK`. The macro
    duplication audit is unfinished for `WRITE_NOVA_SLOTS`, `LR`/`LT`,
    `EMIT`/`EMITI`/`PUTBYTE`, `WALK_ONE`/`WALK_NOVA`, `LINK_OP`/`UNLINK_OP`.
14. **`ALIAS`/`OWN`/`FREE` self-hosted dispatch:** there are no real top-level
    uses (the last check found zero), so they are not yet verified to have a
    working path like the one `SPAN` needed.
15. **`BS_LAST_NOVA`:** narrowed to "last NOVA surviving a `NOLINK`". The two
    `NOLINK` handlers capture `RA_MC_PREV` before clearing it. Nothing consumes it
    yet. It is kept on purpose as infrastructure. `check_autolink_invariants.py`
    guards against the old reseed/writeback no-op round trip coming back.
16. **Superseded leads, recorded so nobody re-chases them blindly:**
    - The 2026-06-19 call-stack imbalance (`RA_SP` underflow at `RREDI_RET`,
      `macros.re:86`, while processing `wax.re`) disappeared with later fixes and
      was never root-caused. Untested hypotheses: the manual `RA_MC_TMP_RL`
      save/restore in `WIRE_AUTOLINK`/`RREDI` duplicates the automatic stack; the
      self-referential `RREDI RREDI_RET` and `RREDI WAR_RET` should use raw `NOVA X
      Redi El1=RA_LINK`, as `AUTOLINK_RET`/`AUTORST_RET` do; the frame size is
      inconsistent. A `LOAD_CMD_R_GROUP` fast-path for RVOCA/RREDI regressed
      33 -> 1 and was reverted.
    - In the twenty-second round, `SK_DECLARE_ONLY` read 1 during what looked like
      Wave 1. The Pattern 9 allowlist fix overtook the question, and it was never
      explicitly resolved.
    - Retracted in rounds 17-20 (they looked like causes but were not): the
      `SK_FIDX`/`RA_LOAD_FD` desync, header text re-read from a rewound or stale
      buffer position, and "the whole file loads in one read". The real issue was
      an empty token after a block comment flowing into `LOAD_RKVA_EMCK` inside
      `LOAD_READARG_KV`'s argument search.

---

## Resolved bugs worth remembering (condensed)

Most fixed bugs are listed as instances of the patterns above. These are the
remaining ones that teach something.

- **"OUTER register" dead idiom** (`yaku.re`): return addresses were saved by
  hand into `RA_xxx_OUTER` before `Jump`, and nothing ever read them. Replaced
  with `RCALL_AT name sub landing`, which works with the automatic call stack.
  `LINK` always writes extra lumina (`_add_lumen`), **never** the compact `Next`
  slot, so `LINK X Next Y` does nothing useful. `EM_VS_OUTER` (saving and
  restoring `S_RA_LINK`) was dead too.
- **`ALLOC_TO` needed a Python fast path** (`_wave2_line`), like
  `JZ/JEQ/CLEAR/NOP/RCALL_AT`. `BS_INTERN` uses it very early, long before
  LOAD_MAIN reaches `macros.re`. It wires `Move count->RA_ALLOC_COUNT; Voca
  ALLOC_LUCES; Move RA_ALLOC_RESULT->dest` and registers `name/_J/_K` in
  `_scan_nova_names`.
- **`constants.re` ordering:** `NEWREF C_7_REF C_7` sat between `C_7` and `C_8`,
  and `C_10..C_13` were out of order. That broke the `addr(C_N) = addr(C_0) + 2N`
  invariant that `--invariants` relies on, with cascading warnings up to
  `C_5381`. The post-LOAD_MAIN `NEWREF` cross-ref restore in `loader.py` scanned
  only `macros.re`. It now scans every file ("universal over special-cased").
- **`fmt.re` `SWITCH`** used `val dest` where the self-hosted version required
  `val > dest`. This is superseded by the no-arrow support described above.
- **`HT_NC_BYTES` label/command order:** `HT_NC_BYTES  JEQ HT_NC_DONE ...` put
  the label before the command. The loop-back `Jump` also targeted the wrong
  label. Found by comparing with the parallel `HT_NAME_STORE`. Checker:
  `check_label_order.py` (JEQ/JZ).
- **"LOAD_CU_BUILDER dispatch path is dead code"** (this title is cited by
  REFERENCE.md): the native-vs-builder split in `LOAD_CMD_UNKNOWN` never matched
  anything. It was removed in the eleventh round, together with the
  `LOAD_CU_BUILDER`/`LOAD_CU_CALL` labels. Removing it exposed the
  `SK_FLAG`/`RA_BS_FLAG` bug (Pattern 12). REFERENCE.md's note that
  `RCALL_AT`/`EMIT`/`PUTBYTE` give `op=0` is explained by Pattern R, not by this
  branch.
- **`BS_LAST_NOVA` reseed/writeback was a no-op round trip:** `LOAD_CU_BMAR`
  captured `RA_MC_PREV` before the call and `LOAD_CU_BSYNC` wrote the same value
  back. It was unnecessary anyway, because `RA_MC_PREV` flows forward across
  nested calls, like Python's `_last_nova`. Removed (open issue 15).
- **HT_NPI_SET init never reached:** a load-time `SEAL` shortcut was the wrong
  category of fix and caused a regression. The fix was to put the runtime init
  into LOAD_MAIN's guaranteed bootstrap sequence. Fix the right *kind* of
  problem.
- **Cosmetic:** `gen_compiler.py` reported "0 store instructions" because it
  compared lines for equality with `'  store'`. It now uses `startswith`. The
  lesson: a user's report that "this number looks wrong" is worth checking even
  when the artifact is fine.
- **`Interpreter` `__slots__`** broke instance monkeypatching in `trace.py
  --loadmain` (see tooling).

---

## Debugging and tooling lessons

- **A hardcoded path plus a silent "file not found" skip is a layout bug waiting
  to happen.** When the sources moved into subdirectories, `loader.py` kept
  opening `<root>/commands.re`, found nothing and skipped without a word. Every
  `Cmd=` command declaration went missing (5875 -> 5783 symbols, lint still
  clean). At the same time `iris.py` swallowed the `ModuleNotFoundError` from
  its own modules (loader not on `sys.path`) and showed 6 commands instead of
  32. Fix: scan every `.re` recursively for declarations, and let a module
  that exists but fails to import fail loudly. Verify a layout change by
  building the same sources both ways and comparing symbol counts.

### Trust your instruments first

- **Validate a probe against a positive control** before trusting a zero or
  low result. For example, confirm that `BS_LOOKUP` fires thousands of times.
  Skipping this caused several wrong conclusions in Pattern Z.
- **Check whether `execute_aether` returned exactly `max_steps`.** If it did,
  the run was cut off and "nothing happened" means nothing. Isolated harnesses
  need realistic budgets (`HT_CLEAR` alone takes about 460k steps).
- **Patched traces must use the production step limit.** A "34/42 vs 42/42"
  discrepancy came from patched `_run` copies that had no step limit and so ran
  further than `freeze()` does.
- **`Voca`/`Redi`/`Jump`/`End` are inlined in `interpreter.py`'s `_run`.**
  Monkeypatching `_voca`/`_redi` does nothing on the fast path. Use `trace.py`'s
  `make_patched_run`, which replaces `self._dispatch[op]` inside a replacement
  `_run`, or a full `_run` replacement. Do not re-derive raw patching. `_run` has
  more than one branch, so make sure you patch the one `freeze()` actually uses.
- **`Interpreter` uses `__slots__`:** instance-level patching fails silently.
  Patch at class level (`type(interp).X = f`), and capture the *unbound*
  original, not the bound one (otherwise `self` is passed twice).
- **`load_symbols()` returns `{}` if `reca.sym` is missing**, so every callback
  silently never fires. It looked like the event never happens. `make_patched_run`
  now forces a build and raises on unknown names.
- **Staleness:** `load_or_freeze()` used to thaw `reca.bin` whenever it existed.
  It now uses `_bin_is_stale()` (mtime against every `.re` file and the core
  `.py` files). `_load_fast`'s cache also re-checks staleness on every call.
- **A tool can silently not run at all.** `diag.py`'s CLI dispatcher sat inside
  `mode_invariants()` after its `return`, with no `__main__` guard, so every flag
  did nothing and exited 0. That may be how an unexplained 42 -> 14 regression
  went unnoticed. `check_comments.py` flags `--threshold`/`--spans-only` were
  documented but never parsed. `diag.py --invariants` item 11 printed nothing at
  all. Check that a tool produces output before believing its silence.
- **Tools that reimplement core algorithms drift.** `diag.py --htable` used an XOR
  hash. `mode_load_main` hardcoded `cursor_start=700_000`, zeroed memory, and
  treated `next==0` as halt. Re-running LOAD_MAIN on a frozen snapshot produces
  artifacts ("word = self + NOVA_SIZE"). Trust only direct state inspection after
  one real `freeze()` (`fresh_loader()`), and use re-run tools only for a rough
  "where does it crash".
- **Tracked-label lists go stale.** `mode_load_main` watched removed labels and
  reported 0.
- **`BS_TOKBUF_BASE` is a pointer.** Dereference twice to read the token.
- **Identify files by the `openat` path, not a counter** (Pattern 7).
- **Correlating a hash value with an event misleads** for names that are
  referenced everywhere (op names). Count dispatches to the handler instead.
- **A Python simulation of an algorithm reproduces the *intended* algorithm**,
  not the real wiring. The `lexer.re` label typos showed up only when feeding
  bytes into `BS_READBUF_BASE` and calling `BS_READ_TOKEN` through
  `execute_aether`.
- **A write-watch on `Write` ops misses `Move` with a computed Exit.** Know what
  your probe covers.

### Loops, hangs and performance

- **"A few samples looked different" does not show there is no loop.** A loop
  with a long period looks like progress in short windows. Count something that
  must advance monotonically (`RA_LOAD_RPOS`, `SK_FIDX`, table size) across the
  *whole* run. `trace.py`'s `sample_every` shows what a long run is doing deep
  in.
- **Never raise `max_steps` to make a hang go away.** First distinguish a real
  halt from budget exhaustion (formerly `check_infinite_loop.py`). Raise budgets
  deliberately, as a decision.
- **For "where do the steps go"**, use `fast_profile.py` (a raw dispatch loop
  that counts PC hits only; orders of magnitude faster than `trace.py
  --profile`).
- **For multi-billion-step runs**, use `long_run.py` (resumable checkpoints). It
  stashes the PC and freezes the *entire* aether: the call stack lives at the top
  (`AETHER_SIZE-1`, growing down), and a low watermark drops it. On resume, thaw,
  run `update_relations()` with a symbol table from a small build (addresses are
  deterministic), and continue.
- **Call-stack balance:** track `Voca` with `Exit=RA_LINK` as a push and `Redi`
  with `E1=RA_LINK` as a pop over the whole run, and find the *first* deviation,
  not the last events before the crash (formerly `trace_callstack_depth.py`).

### Tools worth knowing (verify current names in TOOLING.md)

`trace.py` (`make_patched_run`, `--watch-pc`, `--find-writer`, `sample_every`;
`--loadmain` is unreliable because it calls `freeze()` twice and patches after the
fact); `iris.py`, formerly `diag.py` (`--lint`, `--invariants`, `--block-lint`,
`--unwrapped` for decorative lines inside block bodies); `check_comments.py`;
`check_graph_equivalence.py`; `check_duplicate_labels.py`;
`check_deref_mismatch.py`; `check_unreachable_entry.py`;
`check_label_order.py`; `check_lookup_sites.py`; `check_nova_sizing.py`;
`check_op_identity.py`; `check_autolink_invariants.py` (traced: AutoNext
adjacency, and that every `BS_LAST_NOVA` write is followed by clearing
`RA_MC_PREV`); `check_htable_reentrancy.py`; `check_undef_targets.py`;
`run_all_checks.py` (one shared `fresh_loader()` for all checks);
`fast_profile.py`; `long_run.py`. The technique matters more than any single
script: patch `_run`, walk the real instruction stream, log on a specific PC or
condition.

### Working method

- **Trace first, then fix.** Do not add a fix until the exact cause is known.
  Each round of Pattern Z that skipped this cost time.
- **Treat LOAD_MAIN as Reca code.** Its bugs are in `.re` files. When tempted
  by a Python hack, ask what in the Reca code makes the result wrong.
- **Find the existing implementation before writing one.** Examples:
  `CHAIN_START`, `LOAD_INTERN`/`LOAD_APPLY_BACKFILL` (built, never called),
  `PS_LINE`'s design, the `_REF` technique. Grep for the exact label and its
  registration line, not a descriptive name pattern.
- **Mirror Python's reference behaviour** rather than inventing semantics.
  Prefer an existing mechanism to new syntax (the rejected `| NATIVE` marker,
  and the `AddrOf`/`&` idea that became `Idem`).
- **Cheap structural inconsistencies (duplicate labels, mismatched register
  names): fix them immediately.** Spend verification effort in proportion to how
  risky the fix is.
- **Fix the right category of problem** (runtime vs load-time) and the **shared
  primitive** rather than the call site.
- **A real fix often lowers the file count** by exposing a bug that was masked
  before (for example 28 -> 9 after pre-allocation, 39 -> 2 -> 6 after SPAN).
  Investigate before reverting. Also do not assume every drop is progress:
  verify.
- **Re-audit the pattern catalog for new instances systematically.** A
  five-minute "why did removing dead code break this" check found the
  `SK_FLAG`/`RA_BS_FLAG` bug affecting every builder macro.
- **Record retractions explicitly.** When a claim turns out wrong, say so in the
  log rather than deleting it quietly. Several rounds here overstated their
  certainty.
- **Clean before deep debugging:** remove duplication, verify comments
  (`check_comments.py`), and lint for phantom targets. A clean structure makes
  bugs visible.
- **Status claims must say what they measure.** `SK_FIDX` counts files swept,
  not files built correctly. Say "graph-equivalent" or "fixed-point" only when
  that was checked.
