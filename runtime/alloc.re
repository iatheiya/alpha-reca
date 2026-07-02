//alloc.re — Aether allocation subroutines

── Canon ─────────────────────────────────────────────────────────────────────
aether[i] == 0 → absence. aether[i] != 0 → something is here.

── Lux model ────────────────────────────────────────────────────────────────
Data lux:   [lux, l0..ln]    — lux + n lumina, +1 natural absence lux
Instr lux:  [word, op, e1, e2, exit, next, pad]  — 7 physical luces (ITO_SIZE)
Voca/Redi calling convention: ITO slot layout = compact fixed-slot.

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

NOTE: ALLOC_RAW is non-leaf (calls ALLOC_LUCES internally) so that
watermark update logic lives in exactly one place (ALLOC_LUCES).//

── Kernel pointer luces ──────────────────────────────────────────────────────
//These luces follow the double-deref pointer pattern:
  a[K_X] = K_X+1  (stable pointer to value lux)
  a[K_X+1] = actual value
Python freeze() initializes them. Reca Read/Write uses double-deref.//
NEW K_CURSOR     /bump allocator cursor pointer
NEW K_WATERMARK  /high-water mark pointer
NEW K_TRACE_POS  /trace position pointer (0 = tracing off)
NEW K_STACK_TOP  /initial stack pointer value (AETHER_SIZE - 1); written at freeze time
NEW K_FLUX_CURSOR /flux zone cursor (bump upward from FLUX_BOTTOM); double-deref pattern
NEW K_FLUX_FREELIST /head of free-list for flux zone; double-deref; 0 = empty list

NEW RA_FLUX_BOTTOM /= FLUX_BOTTOM constant; written at freeze time
NEW RA_FLUX_TOP    /= FLUX_TOP constant; written at freeze time

── Overflow chain ───────────────────────────────────────────────────────────
//OVERFLOW_REL: special rel value marking a chain link between lux luces.
When ADD_LUMEN finds rel==OVERFLOW_REL: follow tgt to overflow lux (alloc if tgt==0).//
NEW OVERFLOW_REL


NEW RA_ALLOC_TMP
NEW RA_ALLOC_TMP2
NEW RA_ALLOC_TMP3
NEW RA_ALLOC_RESULT
NEWREF RA_ALLOC_RESULT_REF RA_ALLOC_RESULT  /word = addr(RA_ALLOC_RESULT); for WAVE El1/Exit/
NEW RA_ALLOC_COUNT
NEWREF RA_ALLOC_COUNT_REF  RA_ALLOC_COUNT   /word = addr(RA_ALLOC_COUNT); for WAVE El1/Exit/

NEW RA_LM_SRC          /ADD_LUMEN / REMOVE_LUMEN: source lux addr/
NEW RA_LM_REL          /ADD_LUMEN / REMOVE_LUMEN: relation addr (0 = untyped)/
NEW RA_LM_EXIT          /ADD_LUMEN / REMOVE_LUMEN: target lux addr/
NEW RA_LM_OFFSET       /ADD_LUMEN / REMOVE_LUMEN: offset from src to first lumen-pair slot (1=Data lux, ITO_SIZE=ITO lux)/

NEW RA_NL_PREV         /LINK_NEXT: address of the previous ITO lux (0 = none, no-op)/
NEW RA_NL_NEXT         /LINK_NEXT: address of the next ITO lux to link to/
NEW RA_NL_TMP          /LINK_NEXT: scratch/

── ALLOC_LUCES: bump-allocate RA_ALLOC_COUNT luces ──────────────────────────
//OUT: RA_ALLOC_RESULT = base address.
Updates K_CURSOR and K_WATERMARK. Leaf.
Note: guard against flux zone overflow is handled at Python level (loader.py).
Reca-level guard omitted during bootstrap — RA_FLUX_BOTTOM not initialized yet.//
NOLINK
ITO ALLOC_LUCES  Read  El1=K_CURSOR          Exit=RA_ALLOC_RESULT
ITO AC_NEW       Add   El1=RA_ALLOC_RESULT   El2=RA_ALLOC_COUNT  Exit=RA_ALLOC_TMP
ITO AC_WRCUR     Write El1=K_CURSOR          El2=RA_ALLOC_TMP
/Update watermark if new cursor > current watermark
ITO AC_WM_CUR    Read  El1=K_WATERMARK       Exit=RA_ALLOC_TMP2
ITO AC_WM_CK     ULess   El1=RA_ALLOC_TMP2    El2=RA_ALLOC_TMP  Exit=RA_ALLOC_TMP3
ITO AC_WM_CKJ    JumpIf   El1=RA_ALLOC_TMP3   Exit=AC_WM_SET
RREDI AC_RET
NOLINK
ITO AC_WM_SET    Write El1=K_WATERMARK El2=RA_ALLOC_TMP
RREDI AC_RET2

── ALLOC_FLUX_ZONE: allocate RA_ALLOC_COUNT luces in flux zone ───────────────
//First searches free-list for a block of exact size (first-fit).
If found: unlinks and returns it. If not: bump-allocates from flux zone.
Free-list block layout: [size, next_free_ptr, ...data...]
OUT: RA_ALLOC_RESULT = base address in flux zone.
Guard: halts if bump cursor exceeds FLUX_TOP.//
NEW RA_FX_TMP    /scratch for ALLOC_FLUX_ZONE
NEW RA_FX_TMP2   /scratch
NEW RA_FLUX_PREV   /previous free-list lux during scan
NEW RA_FLUX_PREV2  /scratch for prev+1 ptr
NOLINK
    /Check free-list: scan for block of matching size
    ITO ALLOC_FLUX_ZONE Read  El1=K_FLUX_FREELIST  Exit=RA_FX_TMP   /head of list
    JZ AH_FL_EMPTY RA_FX_TMP AH_BUMP          /list empty → bump
    CLEAR AH_FL_INIT RA_FLUX_PREV
    NOLINK
    /Scan: RA_FX_TMP = current block, RA_FLUX_PREV = previous
    ITO AH_FL_LOOP   Read  El1=RA_FX_TMP      Exit=RA_FX_TMP2  /read size
    JEQ AH_FL_CK RA_FX_TMP2 RA_ALLOC_COUNT AH_FL_FOUND
    /size mismatch: advance
    ITO AH_FL_NEXT   Add   El1=RA_FX_TMP El2=C_1 Exit=RA_FLUX_PREV2
    ITO AH_FL_NRD    Read  El1=RA_FLUX_PREV2    Exit=RA_FX_TMP   /next ptr
    JZ AH_FL_NZ RA_FX_TMP AH_BUMP             /end of list → bump
    ITO AH_FL_PREV   Move  El1=RA_FLUX_PREV2    Exit=RA_FLUX_PREV
    ITO AH_FL_LB     Jump  Exit=AH_FL_LOOP
    /Found: unlink from free-list
    NOLINK
    ITO AH_FL_FOUND  Move  El1=RA_FX_TMP      Exit=RA_ALLOC_RESULT
    /Read next_ptr of found block
    ITO AH_FL_FNP    Add   El1=RA_FX_TMP El2=C_1 Exit=RA_FX_TMP2
    ITO AH_FL_FNRD   Read  El1=RA_FX_TMP2     Exit=RA_FX_TMP2
    /Patch: prev.next = found.next (or update head if prev==0)
    JZ AH_FL_PRZ RA_FLUX_PREV AH_FL_SETHEAD
    ITO AH_FL_PPATCH Add   El1=RA_FLUX_PREV El2=C_1 Exit=RA_FLUX_PREV2
    ITO AH_FL_PW     Write El1=RA_FLUX_PREV2    El2=RA_FX_TMP2
    RREDI AH_FL_DONE_r
    NOLINK
    ITO AH_FL_SETHEAD Write El1=K_FLUX_FREELIST El2=RA_FX_TMP2
    RREDI AH_FL_HDONE_r
    /Bump allocation
    NOLINK
    ITO AH_BUMP      Read  El1=K_FLUX_CURSOR     Exit=RA_ALLOC_RESULT
    ITO AH_NEW       Add   El1=RA_ALLOC_RESULT   El2=RA_ALLOC_COUNT  Exit=RA_FX_TMP
    /Guard: if new cursor > FLUX_TOP → halt
    ITO AH_GUARD_CK  ULess El1=RA_FLUX_TOP       El2=RA_FX_TMP     Exit=RA_ALLOC_TMP3
    JZ AH_GUARD_JZ RA_ALLOC_TMP3 AH_WRCUR
    ITO AH_GUARD_HALT End
    NOLINK
    ITO AH_WRCUR     Write El1=K_FLUX_CURSOR     El2=RA_FX_TMP
    RREDI AH_BUMP_RET_r

── FREE_FLUX_ZONE: return RA_ALLOC_RESULT block of RA_ALLOC_COUNT luces to free-list
//Prepends block to free-list. Block layout: [size, next_ptr, ...].
IN: RA_ALLOC_RESULT = block addr, RA_ALLOC_COUNT = block size.//
NOLINK
    /Write size into block[0]
    ITO FREE_FLUX_ZONE Write El1=RA_ALLOC_RESULT  El2=RA_ALLOC_COUNT
    /Write current head into block[1]
    ITO FH_NP        Add   El1=RA_ALLOC_RESULT  El2=C_1  Exit=RA_FX_TMP
    ITO FH_RD_HEAD   Read  El1=K_FLUX_FREELIST  Exit=RA_FX_TMP2
    ITO FH_WNP       Write El1=RA_FX_TMP      El2=RA_FX_TMP2
    /Update head = this block
    ITO FH_WHEAD     Write El1=K_FLUX_FREELIST  El2=RA_ALLOC_RESULT

── ALLOC_LUX: allocate minimal data lux [lux | 0] = 2 physical luces ───────
/OUT: RA_ALLOC_RESULT = lux addr. Caller writes lux at aether[result]. Non-leaf.
NOLINK
    ITO ALLOC_LUX  Move  El1=C_2      Exit=RA_ALLOC_COUNT
    RVOCA ALX_CALL ALLOC_LUCES

── ALLOC_LUX1: allocate data lux with room for 1 lumen = 4 luces [lux|rel|exit|0]
/OUT: RA_ALLOC_RESULT = lux addr. Non-leaf.
NOLINK
    ITO ALLOC_LUX1  Move  El1=C_4      Exit=RA_ALLOC_COUNT
    RVOCA ALX1_CALL ALLOC_LUCES

── ALLOC_ITO: allocate instruction lux = 7 physical luces ─────────────────
//[word, op, e1, e2, exit, next, pad]
OUT: RA_ALLOC_RESULT = lux addr. Non-leaf.//
NOLINK
    ITO ALLOC_ITO  Move  El1=ITO_SIZE Exit=RA_ALLOC_COUNT
    RVOCA AI_CALL  ALLOC_LUCES

── ALLOC_LUX_N: allocate data lux with RA_AN_NLUMINA lumina ───────────────
//IN:  RA_AN_NLUMINA = number of lumina (n)
Physical size = 1 (word) + 2*n (lumen pairs) + 2 (overflow sentinel) = 2*n + 3
Last two slots = [OVERFLOW_REL, 0] — overflow chain sentinel.
ADD_LUMEN treats OVERFLOW_REL as overflow link, not absence.
OUT: RA_ALLOC_RESULT = lux addr. Non-leaf.//
NEW RA_AN_NLUMINA      /input: number of lumina needed
NOLINK
    ITO ALLOC_LUX_N  Add     El1=RA_AN_NLUMINA El2=RA_AN_NLUMINA Exit=RA_ALLOC_COUNT  /2*n
    ITO ANN_SZ       Add     El1=RA_ALLOC_COUNT El2=C_3 Exit=RA_ALLOC_COUNT  /+1 word +2 overflow
    RVOCA ANN_CALL     ALLOC_LUCES
    /Write OVERFLOW_REL sentinel at slot [1 + 2*n] (after all lumen pairs)
    ITO ANN_OVPOS    Add     El1=RA_ALLOC_RESULT El2=RA_AN_NLUMINA Exit=RA_ALLOC_TMP
    ITO ANN_OVPOS2   Add     El1=RA_ALLOC_TMP   El2=RA_AN_NLUMINA Exit=RA_ALLOC_TMP
    ITO ANN_OVPOS3   Add     El1=RA_ALLOC_TMP   El2=C_1           Exit=RA_ALLOC_TMP
    ITO ANN_OVWR     Write   El1=RA_ALLOC_TMP   El2=OVERFLOW_REL
    /slot [1 + 2*n + 1] stays 0 (bump zeroes) = no overflow yet

── ADD_LUMEN: append (RA_LM_REL, RA_LM_EXIT) pair to lux RA_LM_SRC ──────────
//Lumen model: each lumen = 2 luces [rel, exit]. Absence = rel==0.
Overflow: rel==OVERFLOW_REL means chain link. overflow_lux==0 → allocate new and link.
overflow_lux!=0 → follow to existing overflow lux and continue scanning there.
Walks pairs from pos RA_LM_OFFSET. At OVERFLOW_REL: follow chain (create if needed).
REQUIRES: lux allocated with ALLOC_LUX_N or equivalent with overflow sentinel.
REQUIRES: caller sets RA_LM_OFFSET = offset from src to first lumen-pair slot
(1 for a Data lux, ITO_SIZE for an ITO lux — see RA_LM_OFFSET declaration below).
Non-leaf.//
NEW RA_AL_POS      /current scan position (points to rel lux of each pair)/
NEW RA_AL_OV_POS   /position of OVERFLOW_REL slot (for writing new overflow addr)/
NOLINK
    /Guard: skip if src == 0
    JZ AL_SRCCK RA_LM_SRC AL_DONE

    /Start scan from pos RA_LM_OFFSET (skip lux's own header fields)
    ITO ADD_LUMEN    Add       El1=RA_LM_SRC El2=RA_LM_OFFSET Exit=RA_AL_POS

    /Loop: read rel at current pos
    NOLINK
    ITO AL_LOOP      Read   El1=RA_AL_POS Exit=RA_ALLOC_TMP
    /rel==0: absence marker → write lumen here
    JZ AL_CKZ RA_ALLOC_TMP AL_WRITE
    /rel==OVERFLOW_REL: chain link
    JEQ AL_OVCK RA_ALLOC_TMP OVERFLOW_REL AL_OVERFLOW
    /normal rel: advance by 2
    ITO AL_ADV       Add       El1=RA_AL_POS El2=C_2 Exit=RA_AL_POS
    ITO AL_LB        Jump      Exit=AL_LOOP

    /OVERFLOW_REL found: check overflow_lux
    NOLINK
    ITO AL_OVERFLOW  Add       El1=RA_AL_POS El2=C_1 Exit=RA_AL_OV_POS
    ITO AL_OV_READ   Read      El1=RA_AL_OV_POS Exit=RA_ALLOC_TMP
    /overflow_lux!=0: follow to existing overflow lux
    JZ AL_OVTGTZ RA_ALLOC_TMP AL_OV_ALLOC
    /Continue scanning in overflow lux from pos 1
    ITO AL_OV_CONT   Add       El1=RA_ALLOC_TMP El2=C_1 Exit=RA_AL_POS
    ITO AL_OV_JMP    Jump      Exit=AL_LOOP

    /overflow_lux==0: allocate new lux with room for 4 lumina + sentinel
    NOLINK
    ITO AL_OV_ALLOC  Move      El1=C_4    Exit=RA_AN_NLUMINA
    RVOCA AL_OV_AN     ALLOC_LUX_N
    /Link: write new overflow lux addr into the overflow slot
    ITO AL_OV_LINK   Write     El1=RA_AL_OV_POS El2=RA_ALLOC_RESULT
    /Continue in new overflow lux from pos 1
    ITO AL_OV_NEW    Add       El1=RA_ALLOC_RESULT El2=C_1 Exit=RA_AL_POS
    ITO AL_OV_NEWJMP Jump      Exit=AL_LOOP

    /Write [rel, exit] at the absence position
    NOLINK
    ITO AL_WRITE     Write  El1=RA_AL_POS El2=RA_LM_REL
    ITO AL_TPOS      Add       El1=RA_AL_POS El2=C_1 Exit=RA_ALLOC_TMP
    ITO AL_WRITET    Write  El1=RA_ALLOC_TMP El2=RA_LM_EXIT
    /pos+2 has OVERFLOW_REL sentinel (written by ALLOC_LUX_N) — chain preserved
    NOLINK
    RREDI AL_DONE
── REMOVE_LUMEN: remove pair (RA_LM_REL, RA_LM_EXIT) from lux RA_LM_SRC ─────
//Finds first pair where rel==RA_LM_REL AND exit==RA_LM_EXIT.
Shifts remaining pairs left by 2 luces. Zeroes the last 2 luces.
Follows overflow chain (OVERFLOW_REL) transparently.
REQUIRES: caller sets RA_LM_OFFSET = offset from src to first lumen-pair slot
(1 for a Data lux, ITO_SIZE for an ITO lux — see RA_LM_OFFSET declaration below).
Non-leaf.//
NEW RA_RL_POS      /current scan position (rel lux of pair)
NEW RA_RL_NEXT     /source position during shift
NEW RA_RL_VAL      /temp value
NOLINK
NOLINK
    /Guard: skip if src == 0
    JZ REMOVE_LUMEN RA_LM_SRC RL_DONE

    /Start scan from pos RA_LM_OFFSET (rel lux of first pair)
    ITO RL_INIT     Add       El1=RA_LM_SRC El2=RA_LM_OFFSET Exit=RA_RL_POS

    /Find pair where rel==RA_LM_REL AND exit==RA_LM_EXIT
    NOLINK
    ITO RL_LOOP     Read   El1=RA_RL_POS Exit=RA_RL_VAL          /read rel
    JZ RL_CKZ RA_RL_VAL RL_DONE
    /Follow overflow chain
    JEQ RL_OVCK RA_RL_VAL OVERFLOW_REL RL_OVERFLOW
    JEQ RL_CREL RA_RL_VAL RA_LM_REL RL_CKTGT
    ITO RL_ADV      Add       El1=RA_RL_POS El2=C_2 Exit=RA_RL_POS /step over pair
    ITO RL_LB       Jump      Exit=RL_LOOP
    /Overflow: follow chain to next lux
    NOLINK
    ITO RL_OVERFLOW Add       El1=RA_RL_POS El2=C_1 Exit=RA_RL_NEXT
    ITO RL_OV_READ  Read      El1=RA_RL_NEXT Exit=RA_RL_VAL
    JZ RL_OVZ RA_RL_VAL RL_DONE  /no overflow lux → not found
    ITO RL_OV_CONT  Add       El1=RA_RL_VAL El2=C_1 Exit=RA_RL_POS
    ITO RL_OV_JMP   Jump      Exit=RL_LOOP
    NOLINK
    ITO RL_CKTGT    Add       El1=RA_RL_POS El2=C_1 Exit=RA_RL_NEXT
    ITO RL_RTGT     Read   El1=RA_RL_NEXT Exit=RA_RL_VAL          /read exit
    JEQ RL_CKT RA_RL_VAL RA_LM_EXIT RL_FOUND
    ITO RL_TADV     Add       El1=RA_RL_POS El2=C_2 Exit=RA_RL_POS
    ITO RL_TLB      Jump      Exit=RL_LOOP

    /Shift remaining pairs left by 2 starting from found pos
    NOLINK
    ITO RL_FOUND    Add       El1=RA_RL_POS El2=C_2 Exit=RA_RL_NEXT /src = pos+2

    NOLINK
    ITO RL_SHIFT    Read   El1=RA_RL_NEXT Exit=RA_RL_VAL
    JZ RL_SHZ RA_RL_VAL RL_ZERO
    /Don't shift past OVERFLOW_REL — stop there
    JEQ RL_SHOVCK RA_RL_VAL OVERFLOW_REL RL_ZERO
    ITO RL_SHW      Write  El1=RA_RL_POS El2=RA_RL_VAL             /copy rel
    ITO RL_SHADV1   Add       El1=RA_RL_NEXT El2=C_1 Exit=RA_RL_NEXT
    ITO RL_SHADV2   Add       El1=RA_RL_POS  El2=C_1 Exit=RA_RL_POS
    ITO RL_RTGT2    Read   El1=RA_RL_NEXT Exit=RA_RL_VAL
    ITO RL_SHW2     Write  El1=RA_RL_POS  El2=RA_RL_VAL             /copy exit
    ITO RL_SHADV3   Add       El1=RA_RL_NEXT El2=C_1 Exit=RA_RL_NEXT
    ITO RL_SHADV4   Add       El1=RA_RL_POS  El2=C_1 Exit=RA_RL_POS
    ITO RL_SHLB     Jump      Exit=RL_SHIFT

    /Zero last 2 luces (new absence — rel=0 at RA_RL_POS, exit=0 at pos+1)
    NOLINK
    ITO RL_ZERO     Write  El1=RA_RL_POS El2=C_0
    ITO RL_ZERO2    Add       El1=RA_RL_POS El2=C_1 Exit=RA_RL_NEXT
    ITO RL_ZERO3    Write  El1=RA_RL_NEXT El2=C_0
    NOLINK
    RREDI RL_DONE
── LINK_NEXT: write RA_NL_NEXT into SLOT_NEXT of RA_NL_PREV — only if not adjacent ──
//If RA_NL_PREV==0: no previous instruction in the autolink chain, nothing to do.
If RA_NL_NEXT == RA_NL_PREV + ITO_SIZE: physically adjacent in Aether — leave
SLOT_NEXT at 0 (already 0 from bump-allocation), so the interpreter's implicit
fall-through (pc+ITO_SIZE) applies with zero runtime cost. This is the common
case for ordinary sequential code. Otherwise: write the explicit link.
Shared by saku.re's native LOAD_MAIN dispatch and macros.re's AUTOLINK —
one rule, not duplicated formulas. Leaf.//
NOLINK
    JZ LN_PVCK RA_NL_PREV LN_DONE
    ITO LINK_NEXT   Add   El1=RA_NL_PREV El2=ITO_SIZE   Exit=RA_NL_TMP
    ITO LN_EQ       Equal El1=RA_NL_TMP  El2=RA_NL_NEXT  Exit=RA_NL_TMP
    JZ LN_ADJCK RA_NL_TMP LN_NSLOT
    RREDI LN_DONE
    NOLINK
    ITO LN_NSLOT    Add   El1=RA_NL_PREV El2=SLOT_NEXT   Exit=RA_NL_TMP
    ITO LN_NW       Write El1=RA_NL_TMP  El2=RA_NL_NEXT
    RREDI LN_WRET
── ALLOC_RAW: bump-allocate RA_ALLOC_COUNT luces, zeroed ────────────────────
//Calls ALLOC_LUCES (cursor + watermark), then zeroes the region. Non-leaf.
OUT: RA_ALLOC_RESULT = base address.//
NOLINK
    RVOCA ALLOC_RAW    ALLOC_LUCES
    /RA_ALLOC_RESULT = base; RA_ALLOC_TMP = base + count (new cursor)
    /Restructured loop: compute flag before entry, check at top.
    ITO AR_ZINIT_GO  Move     El1=RA_ALLOC_RESULT Exit=RA_ALLOC_TMP2
    ITO AR_ZINIT_EQ  Equal    El1=RA_ALLOC_TMP2   El2=RA_ALLOC_TMP Exit=RA_FLAG
    ITO AR_ZLOOP     JumpIf   El1=RA_FLAG          Exit=AR_ZDONE
    ITO AR_ZWRITE    Write    El1=RA_ALLOC_TMP2    El2=C_0
    ITO AR_ZINC      Add      El1=RA_ALLOC_TMP2    El2=C_1 Exit=RA_ALLOC_TMP2
    ITO AR_ZEQFLAG   Equal    El1=RA_ALLOC_TMP2    El2=RA_ALLOC_TMP Exit=RA_FLAG
    ITO AR_ZLB       Jump     Exit=AR_ZLOOP
    NOLINK
    RREDI AR_ZDONE
── ACTIVATE_AT: scatter activation ──────────────────────────────────────────
//IN:  RA_ALLOC_RESULT = address to write self-ref (lux = addr).
OUT: aether[addr] = addr. K_WATERMARK updated. Leaf.//
NOLINK
ITO ACTIVATE_AT  Write El1=RA_ALLOC_RESULT El2=RA_ALLOC_RESULT
ITO AT_WM_CUR    Read  El1=K_WATERMARK     Exit=RA_ALLOC_TMP
ITO AT_WM_CK     ULess   El1=RA_ALLOC_TMP    El2=RA_ALLOC_RESULT Exit=RA_ALLOC_TMP2
ITO AT_WM_CKJ    JumpIf   El1=RA_ALLOC_TMP2   Exit=AT_WM_SET
RREDI AT_RET
NOLINK
ITO AT_WM_SET    Write El1=K_WATERMARK El2=RA_ALLOC_RESULT
RREDI AT_RET2
