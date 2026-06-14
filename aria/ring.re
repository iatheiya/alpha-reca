============================================================
//aria/ring.re — Dynamic circular queue backed by a Lux-linked ring
All subroutines use native Voca/Redi via RA_LINK.
The call stack (RA_SP) is automatic — no per-function bookkeeping needed.

DEPENDENCY: aspects.re, core/constants.re, runtime/regs.re,
aria/symphony.re, runtime/alloc.re//
============================================================

NEWREF RING_NEXT_REL

NEW RA_RING_HEAD
NEW RA_RING_TAIL
NEW RA_RING_COUNT
NEW RA_RING_VAL
NEW RA_RING_TMP
NEW RA_RING_TMP2

── RING_INIT ─────────────────────────────────────────────────
/Non-leaf (calls ALLOC_LUX, LINK_OP).
NOLINK
RVOCA RING_INIT    ALLOC_LUX
ITO RI_SAVHD     Move    El1=RA_ALLOC_RESULT Exit=RA_RING_HEAD
ITO RI_TAIL      Move    El1=RA_RING_HEAD   Exit=RA_RING_TAIL
CLEAR RI_CNT RA_RING_COUNT
LINK_OP RI_LINK RA_RING_HEAD RING_NEXT_REL RA_RING_HEAD
RREDI RI_RET_r

── RING_PUSH ─────────────────────────────────────────────────
/Non-leaf — return is automatic.
NOLINK
WALK_ONE RING_PUSH RA_RING_TAIL RING_NEXT_REL
/RA_SR_OUT = RING_NEXT(TAIL)
JEQ RP_AFTER_WALK RA_SR_OUT RA_RING_HEAD RING_PUSH_GROW
ITO RP_WRITE     Write El1=RA_RING_TAIL  El2=RA_RING_VAL
ITO RP_ADV_SAVE  Move    El1=RA_SR_OUT      Exit=RA_RING_TAIL
ITO RP_CNT       Add     El1=RA_RING_COUNT  El2=C_1 Exit=RA_RING_COUNT
RREDI RP_RET_r

── RING_PUSH_GROW ────────────────────────────────────────────
NOLINK
ITO RING_PUSH_GROW Move  El1=RA_SR_OUT      Exit=RA_RING_TMP
RVOCA RP_GR_NEW      ALLOC_LUX
ITO RP_GR_SAVNEW   Move  El1=RA_ALLOC_RESULT Exit=RA_RING_TMP2
UNLINK_OP RP_GR_UNL RA_RING_TAIL RING_NEXT_REL RA_RING_TMP
LINK_OP RP_GR_LK1 RA_RING_TAIL RING_NEXT_REL RA_RING_TMP2
LINK_OP RP_GR_LK2 RA_RING_TMP2 RING_NEXT_REL RA_RING_TMP
ITO RP_GR_WRITE    Write El1=RA_RING_TAIL El2=RA_RING_VAL
ITO RP_GR_ADVT     Move    El1=RA_RING_TMP2  Exit=RA_RING_TAIL
ITO RP_GR_CNT      Add     El1=RA_RING_COUNT El2=C_1 Exit=RA_RING_COUNT
RREDI RP_GR_RET_r

── RING_POP ──────────────────────────────────────────────────
/Non-leaf — return is automatic.
NOLINK
JEQ RING_POP RA_RING_HEAD RA_RING_TAIL RP_EMPTY
ITO RP_READ      Read El1=RA_RING_HEAD   Exit=RA_RING_VAL
WALK_ONE RP_ADV RA_RING_HEAD RING_NEXT_REL
ITO RP_ADV4      Move    El1=RA_SR_OUT       Exit=RA_RING_HEAD
ITO RP_CNTD      Sub     El1=RA_RING_COUNT   El2=C_1 Exit=RA_RING_COUNT
RREDI RP_RET2_r

CLEAR RP_EMPTY RA_RING_VAL
RREDI RP_EMRET_r

── RING_FULL ─────────────────────────────────────────────────
/Non-leaf — return is automatic.
NOLINK
WALK_ONE RING_FULL RA_RING_TAIL RING_NEXT_REL
ITO RFL_AFTER    Equal El1=RA_SR_OUT El2=RA_RING_HEAD Exit=RA_FLAG
RREDI RFL_RET_r

── RING_EMPTY ────────────────────────────────────────────────
/Leaf.
NOLINK
ITO RING_EMPTY   Equal El1=RA_RING_HEAD El2=RA_RING_TAIL Exit=RA_FLAG
RREDI REM_RET
