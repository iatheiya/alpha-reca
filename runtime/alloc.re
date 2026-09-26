//
alloc.re — Aether allocation subroutines

── Canon ─────────────────────────────────────────────────────────────────────
aether[i] == 0 → absence. aether[i] != 0 → something is here.

── Lux model ────────────────────────────────────────────────────────────────
Data lux:   [lux, l0..ln]    — lux + n lumina, +1 natural absence lux
Instr lux:  [word, op, e1, e2, exit, next, pad]  — 7 physical luces (NOVA_SIZE)
Voca/Redi calling convention: NOVA slot layout = compact fixed-slot.

There are NO separate lumen blocks. Lumina are inline in the lux.
The 0 after the last lumen is automatic (bump zeroes aether) — not a field.

── Dynamic lumen addition ────────────────────────────────────────────────────
ADD_LUMEN: finds the absence marker in src, writes new lumen addr there.
Requires src was allocated with enough space (prepass guarantees this for
bootstrap; runtime callers must pre-allocate). See ALLOC_LUX_N for runtime.

── Calling convention ────────────────────────────────────────────────────────
"RVOCA" to call, "RREDI" to return. The call stack (RA_SP) is automatic —
Voca/Redi(RA_LINK) push/pop the return address, so nested calls work
at any depth with no per-function bookkeeping.

DEPENDENCY: aspects.re  constants.re  registers.re  layout.re

NOTE: ALLOC_RAW is non-leaf (calls ALLOC internally) so that
watermark update logic lives in exactly one place (ALLOC).
//

── Kernel pointer luces ──────────────────────────────────────────────────────
//
These luces follow the double-deref pointer pattern:
  a[K_X] = K_X+1  (stable pointer to value lux)
  a[K_X+1] = actual value
Python freeze() initializes them. Reca Read/Write uses double-deref.
//
NEW K_CURSOR  /bump allocator cursor pointer -- K_ prefix auto-carries SAVE+LOCK
NEW K_WATERMARK  /high-water mark pointer
NEW K_TRACE_POS  /trace position pointer (0 = tracing off)
NEW K_STACK_TOP  /initial stack pointer value (AETHER_SIZE - 1); written at freeze time --
                 /freeze-time write is a direct Python-side poke (loader),
                 /never a Reca Write/Key -- LOCK never sees it, only protects
                 /against a later, accidental Reca-level write
NEW K_FLUX_CURSOR  /flux zone cursor (bump upward from FLUX_BOTTOM); double-deref pattern
NEW K_FLUX_FREELIST  /head of free-list for flux zone; double-deref; 0 = empty list

NEW RA_FLUX_BOTTOM  /= FLUX_BOTTOM constant; written at freeze time
NEW RA_FLUX_TOP  /= FLUX_TOP constant; written at freeze time

── Overflow chain ───────────────────────────────────────────────────────────
//
OVERFLOW_REL: special rel value marking a chain link between lux luces.
When ADD_LUMEN finds rel==OVERFLOW_REL: follow tgt to overflow lux (alloc if tgt==0).
Declared SET (self-ref: word == own addr), not NEW: it is a distinctive marker
value, exactly like ACCORD_USE. The rel==OVERFLOW_REL tests (SR_FIRST_LM /
SR_NEXT_LM / SR_GLX / ADD_LUMEN) compare rel's value against OVERFLOW_REL's word.
As NEW its word was 0, so a normal rel==0 chain terminator matched OVERFLOW_REL
and the lumen walk followed the +1 cell (the next lux's word) as an overflow
target -- straight off the aether on any lux whose neighbour word was large.
A self-ref word is nonzero and never collides with a real rel (no lumen's rel
points at OVERFLOW_REL's own marker lux), so rel==0 now terminates cleanly.
//
SET OVERFLOW_REL


NEW RA_ALLOC_TMP
NEW RA_ALLOC_TMP2
NEW RA_ALLOC_TMP3
NEW RA_ALLOC_RESULT
NEW RA_ALLOC_COUNT
NEW RA_ALLOC_BASE  /LOAD_ALLOC_SIZED (parser.re): base slot count for the lux kind being sized (C_1=data, NOVA_SIZE=nova)/
NEW RA_ALLOC_TERM_FORCE  /LOAD_ALLOC_SIZED (parser.re): 1=always add the +1 terminator slot, 0=only when lcount>0/
/RA_ALLOC_RESULT_REF/RA_ALLOC_COUNT_REF formerly declared here -- removed
/once NOVAE gained its own Op1=/Op2=/Op3=Idem operand-resolution mode
/(formerly an "Idem:" value prefix, since unified onto LOAD_ELEM's own
/key convention). See BUGS.md Pattern Z, REF_TO_IDEM.md.

NEW RA_LM_SRC  /ADD_LUMEN / REMOVE_LUMEN: source lux addr/
NEW RA_LM_REL  /ADD_LUMEN / REMOVE_LUMEN: relation addr (0 = untyped)/
NEW RA_LM_EXIT  /ADD_LUMEN / REMOVE_LUMEN: target lux addr/
NEW RA_LM_OFFSET  /ADD_LUMEN / REMOVE_LUMEN: offset from src to first lumen-pair slot (1=Data lux, NOVA_SIZE=NOVA lux)/

NEW RA_NL_PREV  /LINK_NEXT: address of the previous NOVA lux (0 = none, no-op)/
NEW RA_NL_NEXT  /LINK_NEXT: address of the next NOVA lux to link to/
NEW RA_NL_TMP  /LINK_NEXT: scratch/

── ALLOC: bump-allocate RA_ALLOC_COUNT luces ──────────────────────────
//
OUT: RA_ALLOC_RESULT = base address.
Updates K_CURSOR and K_WATERMARK. Leaf.
Has a bounds guard (AC_G_*): halts cleanly via End if the new cursor would
reach RA_FLUX_BOTTOM, mirroring ALLOC_FLUX_ZONE's FLUX_TOP guard. Verified:
forcing a[RA_FLUX_BOTTOM] artificially low before LOAD_MAIN starts halts it
at SK_FIDX=1 (first file) instead of running off the aether -- the halt
branch does fire correctly. Never fires in real runs (current cursor peaks
far below RA_FLUX_BOTTOM), which is why it looked untested before. Only
guards the self-hosted LOAD_MAIN pass -- Python's own bootstrap bump
allocator (_bump_alloc) is separate and not covered by this check.

Also has a count=0 guard (AC_G_CNT0), for a distinct reason: count=0
does not advance K_CURSOR at all, so whatever a caller then writes into
the returned "fresh" address collides with the very next real
allocation's own lux. This used to be re-guarded by hand at every
risky call site (found and fixed twice in wax.re, missed a third time
in a since-removed BLOCK macro -- see docs/DECISIONS.md "Wave 2 is one pass" /
PLAN_ARENA_UNIFY.md's broader point about this class of duplication).
Moved here instead, once: no caller needs to know this hazard exists,
today or in the future -- ALLOC is the one place luces are ever
bump-allocated, so it is the one place that has to refuse count=0.
//
NOLINK
NOVA ALLOC Op=Read El1=K_CURSOR Exit=RA_ALLOC_RESULT
JZ AC_G_CNT0 El1=RA_ALLOC_COUNT Exit=AC_G_HALT
NOVA AC_NEW Op=Add El1=RA_ALLOC_RESULT El2=RA_ALLOC_COUNT Exit=RA_ALLOC_TMP
/Guard: halt cleanly if the new cursor would reach the flux zone, instead of
/running off the aether into an IndexError. RA_FLUX_BOTTOM holds the limit at
/single-deref (Python writes a[RA_FLUX_BOTTOM]=FLUX_BOTTOM at freeze), so read
/it with Move and compare directly with ULess -- mirrors ALLOC_FLUX_ZONE's own
/RA_FLUX_TOP guard. Self-disables while the limit is still 0 (early bootstrap).
NOVA AC_G_LIM Op=Move El1=RA_FLUX_BOTTOM Exit=RA_ALLOC_TMP2
JZ AC_G_OFF El1=RA_ALLOC_TMP2 Exit=AC_WRCUR
NOVA AC_G_CK Op=ULess El1=RA_FLUX_BOTTOM El2=RA_ALLOC_TMP Exit=RA_ALLOC_TMP2
JZ AC_G_OK El1=RA_ALLOC_TMP2 Exit=AC_WRCUR
NOVA AC_G_HALT Op=End
NOLINK
NOVA AC_WRCUR Op=Key El1=K_CURSOR El2=RA_ALLOC_TMP
/Update watermark if new cursor > current watermark
NOVA AC_WM_CUR Op=Read El1=K_WATERMARK Exit=RA_ALLOC_TMP2
NOVA AC_WM_CK Op=ULess El1=RA_ALLOC_TMP2 El2=RA_ALLOC_TMP Exit=RA_ALLOC_TMP3
NOVA AC_WM_CKJ Op=JumpIf El1=RA_ALLOC_TMP3 Exit=AC_WM_SET
RREDI AC_RET
NOLINK
NOVA AC_WM_SET Op=Key El1=K_WATERMARK El2=RA_ALLOC_TMP
RREDI AC_RET2

── Region combo blocks (B: In=/K_/RA_ region grouping) ───────────────────────
//
Mirrors loader.py's _reserve_combo_blocks / _bump_alloc_combo. A declared
name's region combo is a bitmask -- OWN=1, SAVE=2, LOCK=4. Names that share a
combo are allocated contiguously from that combo's own cursor, so a LOCK-combo
forms a single address range (the same shape interpreter._lock_ranges
enforces) and a SAVE region is block-savable. Indexed by bitmask 1..7; slot 0
(no region) is never used -- those names take the ordinary flat bump (ALLOC).

The region facts are computed ENTIRELY on the Python side (the source of
truth: _name_regions / _combo_key) and baked into the aether at freeze, the
same pattern as SK_DECL_TABLE / SK_OPENER_TABLE:
  SK_REGION_TABLE  -- [name-hash, bitmask] pairs, one per region name (walked
                      by REGION_LOOKUP; keyed by djb2 hash so it is stable
                      across Python's and LOAD_MAIN's differing layouts).
  SK_COMBO_SIZE    -- total luces per combo bitmask, summed with LOAD_MAIN's
                      own stub sizing (NOVA_SIZE for a nova/naming name, else
                      1), so a reserved block is exactly what the prepass then
                      fills. Reca does NOT re-derive any of this -- no In=/
                      prefix parsing on this side; it only consumes.
Then RESERVE_COMBOS turns each non-zero SK_COMBO_SIZE into one reserved block
and seeds that combo's cursor; the prepass looks a name up with REGION_LOOKUP
and, when it carries a combo, allocates from the cursor via ALLOC_COMBO
(threaded through BS_INTERN_AT's RA_BS_INT_COMBO parameter). Tables are
indexed by Idem-derived base + mask, never by a stored word, so they are
correct regardless of how the table lux was seeded.
//
NEW SK_COMBO_SIZE Count=8  /Python-baked: total luces per combo bitmask
NEW SK_COMBO_CUR Count=8   /bump cursor per combo (= block base after reserve)

NEW RA_CB_MASK  /combo bitmask 1..7 (0 = no region)
NEW RA_CB_N     /size in luces
NEW RA_CB_TMP
NEW RA_CB_TMP2
NEW RA_CB_I     /RESERVE_COMBOS loop index

── REGION_LOOKUP: RA_LOAD_HASH -> RA_CB_MASK (0 if no region) ─
/Walk SK_REGION_TABLE ([hash, mask, hash, mask, ..., 0]) for a name whose
/hash matches RA_LOAD_HASH; return its bitmask, or 0. Leaf. Same walk shape
/as LOAD_CU_PREPARE's SK_DECL_TABLE gate, one level of indirection wider
/(2-wide entries: hash then mask).
NOLINK
NOVA REGION_LOOKUP Op=Move El1=SK_REGION_TABLE_BASE Exit=RA_CB_TMP
CLEAR RGL_INIT El1=RA_CB_MASK
/RGL_LOOP: fall-through here + RGL_JMP back-edge (a label is jumpable without
/a preceding NOLINK; an earlier NOLINK terminated RGL_INIT at LUX_END).
/RGL_ prefix (Region Lookup), distinct from REMOVE_LUMEN's RL_ family below:
/both routines abbreviated to RL_ and collided on RL_INIT/LOOP/ADV/DONE, so
/the second (REMOVE_LUMEN) had its loop back-edges silently resolve here.
NOVA RGL_LOOP Op=Read El1=RA_CB_TMP Exit=RA_CB_TMP2
JZ RGL_ZCK El1=RA_CB_TMP2 Exit=RGL_DONE
JEQ RGL_HITCK El1=RA_CB_TMP2 El2=RA_LOAD_HASH Exit=RGL_HIT
NOVA RGL_ADV Op=Add El1=RA_CB_TMP El2=C_2 Exit=RA_CB_TMP
NOVA RGL_JMP Op=Jump Exit=RGL_LOOP
NOLINK
NOVA RGL_HIT Op=Add El1=RA_CB_TMP El2=C_1 Exit=RA_CB_TMP
NOVA RGL_HITRD Op=Read El1=RA_CB_TMP Exit=RA_CB_MASK
/RGL_DONE: fall-through from RGL_HITRD (hit path) + RGL_ZCK's jump (miss path).
RREDI RGL_DONE

── RESERVE_COMBOS: reserve one block per non-empty combo ─────
/For mask=1..7: if SK_COMBO_SIZE[mask] > 0, bump-allocate that many luces
/(ALLOC) and seed SK_COMBO_CUR[mask] with the block base. Runs once, in
/LOAD_MAIN, before the prepass allocates a single region name.
NOLINK
NOVA RESERVE_COMBOS Op=Move El1=C_1 Exit=RA_CB_I
/RC_LOOP is the loop head: reached by fall-through here (first pass) and by
/RC_JMP's back-edge -- a label is jumpable without a NOLINK in front, so no
/NOLINK here (an earlier NOLINK terminated RESERVE_COMBOS at LUX_END before
/the loop ever ran).
NOVA RC_LOOP Op=Idem El1=SK_COMBO_SIZE Exit=RA_CB_TMP
NOVA RC_SLOT Op=Add El1=RA_CB_TMP El2=RA_CB_I Exit=RA_CB_TMP
NOVA RC_RD Op=Read El1=RA_CB_TMP Exit=RA_CB_N
JZ RC_ZCK El1=RA_CB_N Exit=RC_NEXT
NOVA RC_SETCNT Op=Move El1=RA_CB_N Exit=RA_ALLOC_COUNT
RVOCA RC_ALLOC Body=ALLOC
NOVA RC_CURBASE Op=Idem El1=SK_COMBO_CUR Exit=RA_CB_TMP2
NOVA RC_CURSLOT Op=Add El1=RA_CB_TMP2 El2=RA_CB_I Exit=RA_CB_TMP2
NOVA RC_CURWR Op=Write El1=RA_CB_TMP2 El2=RA_ALLOC_RESULT
/RC_NEXT: reached by fall-through here (the allocated-combo path) and by
/RC_ZCK's jump (the empty-combo path) -- again no NOLINK, same reason.
NOVA RC_NEXT Op=Add El1=RA_CB_I El2=C_1 Exit=RA_CB_I
JEQ RC_ENDCK El1=RA_CB_I El2=C_8 Exit=RC_DONE
NOVA RC_JMP Op=Jump Exit=RC_LOOP
NOLINK
RREDI RC_DONE

── ALLOC_COMBO: bump RA_CB_N luces from combo RA_CB_MASK's cursor ──
/OUT: RA_ALLOC_RESULT = base of the allocation, taken from (and advancing)
/SK_COMBO_CUR[RA_CB_MASK]. Caller guarantees RA_CB_MASK != 0. Leaf.
NOLINK
NOVA ALLOC_COMBO Op=Idem El1=SK_COMBO_CUR Exit=RA_CB_TMP
NOVA ACO_SLOT Op=Add El1=RA_CB_TMP El2=RA_CB_MASK Exit=RA_CB_TMP
NOVA ACO_RD Op=Read El1=RA_CB_TMP Exit=RA_ALLOC_RESULT
NOVA ACO_ADV Op=Add El1=RA_ALLOC_RESULT El2=RA_CB_N Exit=RA_CB_TMP2
NOVA ACO_WR Op=Write El1=RA_CB_TMP El2=RA_CB_TMP2
RREDI ACO_RET

── ALLOC_FLUX_ZONE: allocate RA_ALLOC_COUNT luces in flux zone ───────────────
//
First searches free-list for a block of exact size (first-fit).
If found: unlinks and returns it. If not: bump-allocates from flux zone.
Free-list block layout: [size, next_free_ptr, ...data...]
OUT: RA_ALLOC_RESULT = base address in flux zone.
Guard: halts if bump cursor exceeds FLUX_TOP.
//
NEW RA_FX_TMP  /scratch for ALLOC_FLUX_ZONE
NEW RA_FX_TMP2  /scratch
NEW RA_FLUX_PREV  /previous free-list lux during scan
NEW RA_FLUX_PREV2  /scratch for prev+1 ptr
NOLINK
    /Check free-list: scan for block of matching size
    NOVA ALLOC_FLUX_ZONE Op=Read El1=K_FLUX_FREELIST Exit=RA_FX_TMP /head of list
    JZ AH_FL_EMPTY El1=RA_FX_TMP Exit=AH_BUMP /list empty → bump
    CLEAR AH_FL_INIT El1=RA_FLUX_PREV
    /Scan: RA_FX_TMP = current block, RA_FLUX_PREV = previous
    NOVA AH_FL_LOOP Op=Read El1=RA_FX_TMP Exit=RA_FX_TMP2 /read size
    JEQ AH_FL_CK El1=RA_FX_TMP2 El2=RA_ALLOC_COUNT Exit=AH_FL_FOUND
    /size mismatch: advance
    NOVA AH_FL_NEXT Op=Add El1=RA_FX_TMP El2=C_1 Exit=RA_FLUX_PREV2
    NOVA AH_FL_NRD Op=Read El1=RA_FLUX_PREV2 Exit=RA_FX_TMP /next ptr
    JZ AH_FL_NZ El1=RA_FX_TMP Exit=AH_BUMP /end of list → bump
    NOVA AH_FL_PREV Op=Move El1=RA_FLUX_PREV2 Exit=RA_FLUX_PREV
    NOVA AH_FL_LB Op=Jump Exit=AH_FL_LOOP
    /Found: unlink from free-list
    NOLINK
    NOVA AH_FL_FOUND Op=Move El1=RA_FX_TMP Exit=RA_ALLOC_RESULT
    /Read next_ptr of found block
    NOVA AH_FL_FNP Op=Add El1=RA_FX_TMP El2=C_1 Exit=RA_FX_TMP2
    NOVA AH_FL_FNRD Op=Read El1=RA_FX_TMP2 Exit=RA_FX_TMP2
    /Patch: prev.next = found.next (or update head if prev==0)
    JZ AH_FL_PRZ El1=RA_FLUX_PREV Exit=AH_FL_SETHEAD
    NOVA AH_FL_PPATCH Op=Add El1=RA_FLUX_PREV El2=C_1 Exit=RA_FLUX_PREV2
    NOVA AH_FL_PW Op=Write El1=RA_FLUX_PREV2 El2=RA_FX_TMP2
    RREDI AH_FL_DONE_r
    NOLINK
    NOVA AH_FL_SETHEAD Op=Key El1=K_FLUX_FREELIST El2=RA_FX_TMP2
    RREDI AH_FL_HDONE_r
    /Bump allocation
    NOLINK
    NOVA AH_BUMP Op=Read El1=K_FLUX_CURSOR Exit=RA_ALLOC_RESULT
    NOVA AH_NEW Op=Add El1=RA_ALLOC_RESULT El2=RA_ALLOC_COUNT Exit=RA_FX_TMP
    /Guard: if new cursor > FLUX_TOP → halt
    NOVA AH_GUARD_CK Op=ULess El1=RA_FLUX_TOP El2=RA_FX_TMP Exit=RA_ALLOC_TMP3
    JZ AH_GUARD_JZ El1=RA_ALLOC_TMP3 Exit=AH_WRCUR
    NOVA AH_GUARD_HALT Op=End
    NOLINK
    NOVA AH_WRCUR Op=Key El1=K_FLUX_CURSOR El2=RA_FX_TMP
    RREDI AH_BUMP_RET_r

── FREE_FLUX_ZONE: return RA_ALLOC_RESULT block of RA_ALLOC_COUNT luces to free-list
//
Prepends block to free-list. Block layout: [size, next_ptr, ...].
IN: RA_ALLOC_RESULT = block addr, RA_ALLOC_COUNT = block size.
//
NOLINK
    /Write size into block[0]
    NOVA FREE_FLUX_ZONE Op=Write El1=RA_ALLOC_RESULT El2=RA_ALLOC_COUNT
    /Write current head into block[1]
    NOVA FH_NP Op=Add El1=RA_ALLOC_RESULT El2=C_1 Exit=RA_FX_TMP
    NOVA FH_RD_HEAD Op=Read El1=K_FLUX_FREELIST Exit=RA_FX_TMP2
    NOVA FH_WNP Op=Write El1=RA_FX_TMP El2=RA_FX_TMP2
    /Update head = this block
    NOVA FH_WHEAD Op=Key El1=K_FLUX_FREELIST El2=RA_ALLOC_RESULT
    /Explicit return -- FREE_FLUX_ZONE is currently dead code (never
    /called via RVOCA anywhere in the project), so this fallthrough is
    /currently harmless, but relies on address adjacency exactly like
    /this session's other Pattern Z fixes; fixing now so it isn't a
    /landmine the moment something starts calling it.
    RREDI FH_RET

/ALLOC_LUX/ALLOC_LUX1/ALLOC_NOVA removed: zero real callers anywhere in
/the project. Each was nothing but "Move a fixed literal into
/RA_ALLOC_COUNT, then RVOCA ALLOC" -- exactly what the general
/ALLOC_TO macro (synth.re) already does for any count, literal or not.
/Confirmed via their last real caller: ring.re's RING_INIT still says
/"Non-leaf (calls ALLOC_LUX, LINK_OP)" in its own comment, but the
/actual code there already calls `ALLOC_TO RING_INIT Exit=RA_RING_HEAD
/Count=C_2` -- the real call site was migrated to ALLOC_TO at some
/point and the now-dead wrapper (plus the comment describing it) was
/simply never cleaned up alongside it.

── ALLOC_LUX_N: allocate data lux with RA_AN_NLUMINA lumina ───────────────
//
IN:  RA_AN_NLUMINA = number of lumina (n)
Physical size = 1 (word) + 2*n (lumen pairs) + 2 (overflow sentinel) = 2*n + 3
Last two slots = [OVERFLOW_REL, 0] — overflow chain sentinel.
ADD_LUMEN treats OVERFLOW_REL as overflow link, not absence.
OUT: RA_ALLOC_RESULT = lux addr. Non-leaf.
//
NEW RA_AN_NLUMINA  /input: number of lumina needed
    NOVA ALLOC_LUX_N Op=Add El1=RA_AN_NLUMINA El2=RA_AN_NLUMINA Exit=RA_ALLOC_COUNT /2*n
    NOVA ANN_SZ Op=Add El1=RA_ALLOC_COUNT El2=C_3 Exit=RA_ALLOC_COUNT /+1 word +2 overflow
    RVOCA ANN_CALL Body=ALLOC
    /Write OVERFLOW_REL sentinel at slot [1 + 2*n] (after all lumen pairs)
    NOVA ANN_OVPOS Op=Add El1=RA_ALLOC_RESULT El2=RA_AN_NLUMINA Exit=RA_ALLOC_TMP
    NOVA ANN_OVPOS2 Op=Add El1=RA_ALLOC_TMP El2=RA_AN_NLUMINA Exit=RA_ALLOC_TMP
    NOVA ANN_OVPOS3 Op=Add El1=RA_ALLOC_TMP El2=C_1 Exit=RA_ALLOC_TMP
    NOVA ANN_OVWR Op=Write El1=RA_ALLOC_TMP El2=OVERFLOW_REL
    /slot [1 + 2*n + 1] stays 0 (bump zeroes) = no overflow yet
    /Explicit return -- ALLOC_LUX_N is called via a real RVOCA (see
    /ADD_LUMEN's own AL_OV_AN) whose caller resumes right after with
    /RA_ALLOC_RESULT still needed; relying on fallthrough here landed on
    /ADD_LUMEN's own entry instead once allocation order shifted --
    /confirmed via iris --fallthrough-hijack, same class as this
    /session's other Pattern Z fixes (see BUGS.md).
    RREDI ANN_RET

── ADD_LUMEN: append (RA_LM_REL, RA_LM_EXIT) pair to lux RA_LM_SRC ──────────
//
Lumen model: each lumen = 2 luces [rel, exit]. Absence = rel==0.
Overflow: rel==OVERFLOW_REL means chain link. overflow_lux==0 → allocate new and link.
overflow_lux!=0 → follow to existing overflow lux and continue scanning there.
Walks pairs from pos RA_LM_OFFSET. At OVERFLOW_REL: follow chain (create if needed).
REQUIRES: lux allocated with ALLOC_LUX_N or equivalent with overflow sentinel.
REQUIRES: caller sets RA_LM_OFFSET = offset from src to first lumen-pair slot
(1 for a Data lux, NOVA_SIZE for a NOVA lux — see RA_LM_OFFSET declaration below).
Non-leaf.
//
NEW RA_AL_POS  /current scan position (points to rel lux of each pair)/
NEW RA_AL_OV_POS  /position of OVERFLOW_REL slot (for writing new overflow addr)/
NOLINK
    /Guard: skip if src == 0
    JZ ADD_LUMEN El1=RA_LM_SRC Exit=AL_DONE

    /Start scan from pos RA_LM_OFFSET (skip lux's own header fields)
    NOVA AL_START Op=Add El1=RA_LM_SRC El2=RA_LM_OFFSET Exit=RA_AL_POS

    /Loop: read rel at current pos
    NOVA AL_LOOP Op=Read El1=RA_AL_POS Exit=RA_ALLOC_TMP
    /rel==0: absence marker → write lumen here
    JZ AL_CKZ El1=RA_ALLOC_TMP Exit=AL_WRITE
    /rel==OVERFLOW_REL: chain link
    JEQ AL_OVCK El1=RA_ALLOC_TMP El2=OVERFLOW_REL Exit=AL_OVERFLOW
    /normal rel: advance by 2
    NOVA AL_ADV Op=Add El1=RA_AL_POS El2=C_2 Exit=RA_AL_POS
    NOVA AL_LB Op=Jump Exit=AL_LOOP

    /OVERFLOW_REL found: check overflow_lux
    NOLINK
    NOVA AL_OVERFLOW Op=Add El1=RA_AL_POS El2=C_1 Exit=RA_AL_OV_POS
    NOVA AL_OV_READ Op=Read El1=RA_AL_OV_POS Exit=RA_ALLOC_TMP
    /overflow_lux!=0: follow to existing overflow lux
    JZ AL_OVTGTZ El1=RA_ALLOC_TMP Exit=AL_OV_ALLOC
    /Continue scanning in overflow lux from pos 1
    NOVA AL_OV_CONT Op=Add El1=RA_ALLOC_TMP El2=C_1 Exit=RA_AL_POS
    NOVA AL_OV_JMP Op=Jump Exit=AL_LOOP

    /overflow_lux==0: allocate new lux with room for 4 lumina + sentinel
    NOLINK
    NOVA AL_OV_ALLOC Op=Move El1=C_4 Exit=RA_AN_NLUMINA
    RVOCA AL_OV_AN Body=ALLOC_LUX_N
    /Link: write new overflow lux addr into the overflow slot
    NOVA AL_OV_LINK Op=Write El1=RA_AL_OV_POS El2=RA_ALLOC_RESULT
    /Continue in new overflow lux from pos 1
    NOVA AL_OV_NEW Op=Add El1=RA_ALLOC_RESULT El2=C_1 Exit=RA_AL_POS
    NOVA AL_OV_NEWJMP Op=Jump Exit=AL_LOOP

    /Write [rel, exit] at the absence position
    NOLINK
    NOVA AL_WRITE Op=Write El1=RA_AL_POS El2=RA_LM_REL
    NOVA AL_TPOS Op=Add El1=RA_AL_POS El2=C_1 Exit=RA_ALLOC_TMP
    NOVA AL_WRITET Op=Write El1=RA_ALLOC_TMP El2=RA_LM_EXIT
    /pos+2 has OVERFLOW_REL sentinel (written by ALLOC_LUX_N) — chain preserved
    RREDI AL_DONE
── REMOVE_LUMEN: remove pair (RA_LM_REL, RA_LM_EXIT) from lux RA_LM_SRC ─────
//
Finds first pair where rel==RA_LM_REL AND exit==RA_LM_EXIT.
Shifts remaining pairs left by 2 luces. Zeroes the last 2 luces.
Follows overflow chain (OVERFLOW_REL) transparently.
REQUIRES: caller sets RA_LM_OFFSET = offset from src to first lumen-pair slot
(1 for a Data lux, NOVA_SIZE for a NOVA lux — see RA_LM_OFFSET declaration below).
Non-leaf.
//
NEW RA_RL_POS  /current scan position (rel lux of pair)
NEW RA_RL_NEXT  /source position during shift
NEW RA_RL_VAL  /temp value
NOLINK
NOLINK
    /Guard: skip if src == 0
    JZ REMOVE_LUMEN El1=RA_LM_SRC Exit=RL_DONE

    /Start scan from pos RA_LM_OFFSET (rel lux of first pair)
    NOVA RL_INIT Op=Add El1=RA_LM_SRC El2=RA_LM_OFFSET Exit=RA_RL_POS

    /Find pair where rel==RA_LM_REL AND exit==RA_LM_EXIT
    NOVA RL_LOOP Op=Read El1=RA_RL_POS Exit=RA_RL_VAL /read rel
    JZ RL_CKZ El1=RA_RL_VAL Exit=RL_DONE
    /Follow overflow chain
    JEQ RL_OVCK El1=RA_RL_VAL El2=OVERFLOW_REL Exit=RL_OVERFLOW
    JEQ RL_CREL El1=RA_RL_VAL El2=RA_LM_REL Exit=RL_CKTGT
    NOVA RL_ADV Op=Add El1=RA_RL_POS El2=C_2 Exit=RA_RL_POS /step over pair
    NOVA RL_LB Op=Jump Exit=RL_LOOP
    /Overflow: follow chain to next lux
    NOLINK
    NOVA RL_OVERFLOW Op=Add El1=RA_RL_POS El2=C_1 Exit=RA_RL_NEXT
    NOVA RL_OV_READ Op=Read El1=RA_RL_NEXT Exit=RA_RL_VAL
    JZ RL_OVZ El1=RA_RL_VAL Exit=RL_DONE /no overflow lux → not found
    NOVA RL_OV_CONT Op=Add El1=RA_RL_VAL El2=C_1 Exit=RA_RL_POS
    NOVA RL_OV_JMP Op=Jump Exit=RL_LOOP
    NOLINK
    NOVA RL_CKTGT Op=Add El1=RA_RL_POS El2=C_1 Exit=RA_RL_NEXT
    NOVA RL_RTGT Op=Read El1=RA_RL_NEXT Exit=RA_RL_VAL /read exit
    JEQ RL_CKT El1=RA_RL_VAL El2=RA_LM_EXIT Exit=RL_FOUND
    NOVA RL_TADV Op=Add El1=RA_RL_POS El2=C_2 Exit=RA_RL_POS
    NOVA RL_TLB Op=Jump Exit=RL_LOOP

    /Shift remaining pairs left by 2 starting from found pos
    NOLINK
    NOVA RL_FOUND Op=Add El1=RA_RL_POS El2=C_2 Exit=RA_RL_NEXT /src = pos+2

    NOVA RL_SHIFT Op=Read El1=RA_RL_NEXT Exit=RA_RL_VAL
    JZ RL_SHZ El1=RA_RL_VAL Exit=RL_ZERO
    /Don't shift past OVERFLOW_REL — stop there
    JEQ RL_SHOVCK El1=RA_RL_VAL El2=OVERFLOW_REL Exit=RL_ZERO
    NOVA RL_SHW Op=Write El1=RA_RL_POS El2=RA_RL_VAL /copy rel
    NOVA RL_SHADV1 Op=Add El1=RA_RL_NEXT El2=C_1 Exit=RA_RL_NEXT
    NOVA RL_SHADV2 Op=Add El1=RA_RL_POS El2=C_1 Exit=RA_RL_POS
    NOVA RL_RTGT2 Op=Read El1=RA_RL_NEXT Exit=RA_RL_VAL
    NOVA RL_SHW2 Op=Write El1=RA_RL_POS El2=RA_RL_VAL /copy exit
    NOVA RL_SHADV3 Op=Add El1=RA_RL_NEXT El2=C_1 Exit=RA_RL_NEXT
    NOVA RL_SHADV4 Op=Add El1=RA_RL_POS El2=C_1 Exit=RA_RL_POS
    NOVA RL_SHLB Op=Jump Exit=RL_SHIFT

    /Zero last 2 luces (new absence — rel=0 at RA_RL_POS, exit=0 at pos+1)
    NOLINK
    NOVA RL_ZERO Op=Write El1=RA_RL_POS El2=C_0
    NOVA RL_ZERO2 Op=Add El1=RA_RL_POS El2=C_1 Exit=RA_RL_NEXT
    NOVA RL_ZERO3 Op=Write El1=RA_RL_NEXT El2=C_0
    /Explicit jump to RL_DONE -- NOLINK only blocks Python's own
    /auto-chain bookkeeping, it does not create a real jump; this relied
    /on RL_DONE's RREDI being physically adjacent, which landed on the
    /unrelated LINK_NEXT section instead once allocation order shifted.
    /Same class as this session's other Pattern Z fixes (see BUGS.md).
    NOVA RL_Z3JMP Op=Jump Exit=RL_DONE
    NOLINK
    RREDI RL_DONE
── LINK_NEXT: write RA_NL_NEXT into SLOT_NEXT of RA_NL_PREV — only if not adjacent ──
//
If RA_NL_PREV==0: no previous instruction in the autolink chain, nothing to do.
If RA_NL_NEXT == RA_NL_PREV + NOVA_SIZE: physically adjacent in Aether — leave
SLOT_NEXT at 0 (already 0 from bump-allocation), so the interpreter's implicit
fall-through (pc+NOVA_SIZE) applies with zero runtime cost. This is the common
case for ordinary sequential code. Otherwise: write the explicit link.
Shared by wax.re's native LOAD_MAIN dispatch and synth.re's AUTOLINK —
one rule, not duplicated formulas. Leaf.
//
NOLINK
    JZ LINK_NEXT El1=RA_NL_PREV Exit=LN_DONE
    NOVA LN_START Op=Add El1=RA_NL_PREV El2=NOVA_SIZE Exit=RA_NL_TMP
    NOVA LN_EQ Op=Equal El1=RA_NL_TMP El2=RA_NL_NEXT Exit=RA_NL_TMP
    JZ LN_ADJCK El1=RA_NL_TMP Exit=LN_NSLOT
    RREDI LN_DONE
    NOLINK
    NOVA LN_NSLOT Op=Add El1=RA_NL_PREV El2=SLOT_NEXT Exit=RA_NL_TMP
    NOVA LN_NW Op=Write El1=RA_NL_TMP El2=RA_NL_NEXT
    RREDI LN_WRET
── ALLOC_RAW_SUB: bump-allocate RA_ALLOC_COUNT luces, zeroed ────────────────
//
Calls ALLOC (cursor + watermark), then zeroes the region. Non-leaf.
OUT: RA_ALLOC_RESULT = base address.

Named ALLOC_RAW_SUB, not ALLOC_RAW: ALLOC_RAW is also a *command* (the
ALLOC_RAW builder, whose expansion RVOCAs to this runtime allocator). Python
keeps command and symbol namespaces apart, but the self-hosted SK_HTAB is one
table, so the runtime subroutine and the command word cannot share a name --
this one is the subroutine, the command word ALLOC_RAW resolves to its builder.
//
NOLINK
    RVOCA ALLOC_RAW_SUB Body=ALLOC
    /RA_ALLOC_RESULT = base; RA_ALLOC_TMP = base + count (new cursor)
    /Restructured loop: compute flag before entry, check at top.
    NOVA AR_ZINIT_GO Op=Move El1=RA_ALLOC_RESULT Exit=RA_ALLOC_TMP2
    NOVA AR_ZINIT_EQ Op=Equal El1=RA_ALLOC_TMP2 El2=RA_ALLOC_TMP Exit=RA_FLAG
    NOVA AR_ZLOOP Op=JumpIf El1=RA_FLAG Exit=AR_ZDONE
    NOVA AR_ZWRITE Op=Write El1=RA_ALLOC_TMP2 El2=C_0
    NOVA AR_ZINC Op=Add El1=RA_ALLOC_TMP2 El2=C_1 Exit=RA_ALLOC_TMP2
    NOVA AR_ZEQFLAG Op=Equal El1=RA_ALLOC_TMP2 El2=RA_ALLOC_TMP Exit=RA_FLAG
    NOVA AR_ZLB Op=Jump Exit=AR_ZLOOP
    NOLINK
    RREDI AR_ZDONE
/ACTIVATE_AT removed: zero real callers anywhere in the project, not
/referenced by any PLAN/BUGS note as future work either (checked) --
/genuinely orphaned, not a "shipped but not yet consumed" library
/routine like arena.re/ring.re. Its watermark-update block was also a
/verbatim duplicate of ALLOC's own (same Read/ULess/JumpIf/Write
/shape, different register names) -- removing it removes the
/duplication too, without making ALLOC itself (the hottest path
/in the system, currently a leaf) pay a non-leaf RVOCA/RREDI call just
/to share code with something nothing calls.

── LIST_APPEND: attach a node to the end of a singly-linked list ──
//
IN:  RA_LL_NODE     the node to attach
     RA_LL_NEXTOFF  offset of the node's "next" field
     RA_LL_TAIL_REF address of the cell holding the list's tail
     RA_LL_HEAD_REF address of the cell to receive the node when the
                    list is still empty
Leaf.

Both references are addresses of cells, not the values themselves, so
"the head" can be an ordinary register in one caller and a field inside
another record in the next -- which is exactly the difference between
tide's record list (head is a register) and its token list (head is
field 0 of the record that owns it). Writing through a reference makes
that difference disappear at the call site instead of forcing a second
copy of this routine.

This is the whole of "append": four callers had it written out inline,
six luces each, and every one of them had already had to be repaired for
relying on address adjacency at the same spot -- the jump from the
empty-list branch back to the common tail update. One copy, repaired
once.
//
NEW RA_LL_NODE
NEW RA_LL_NEXTOFF
NEW RA_LL_TAIL_REF
NEW RA_LL_HEAD_REF
NEW RA_LL_TMP
NEW RA_LL_TMP2
SET LIST_APPEND Value=LA_START
NOVA LA_START Op=Read El1=RA_LL_TAIL_REF Exit=RA_LL_TMP
JZ LA_TAILCK El1=RA_LL_TMP Exit=LA_ISFIRST
NOVA LA_PREVNX Op=Add El1=RA_LL_TMP El2=RA_LL_NEXTOFF Exit=RA_LL_TMP2
NOVA LA_PREVW Op=Write El1=RA_LL_TMP2 El2=RA_LL_NODE
NOVA LA_PREVJ Op=Jump Exit=LA_SETTAIL
NOLINK
NOVA LA_ISFIRST Op=Write El1=RA_LL_HEAD_REF El2=RA_LL_NODE
NOVA LA_SETTAIL Op=Write El1=RA_LL_TAIL_REF El2=RA_LL_NODE
RREDI LA_RET
