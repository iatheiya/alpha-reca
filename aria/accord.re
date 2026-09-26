//
============================================================
//
//
aria/accord.re — Convention enforcement + universal Lux scanner
//

//
New layout: SCAN_ALL_LUX reads LUX_REGISTRY (a contiguous block of
//
//
Data Lux addresses written by loader.py at freeze time).
//
//
This is pure data — no type tags, no typed regions, just the same
//
//
information viewed differently.
//

//
DEPENDENCY: aspects.re, core/constants.re, runtime/layout.re,
//
//
aria/lumen.re, runtime/registers.re
//
//
============================================================
//

NEW ACCORD_RULE
SET ACCORD_USE
NEW ACCORD_EXCEPT

NEW ACCORD_STABLE
LINK ACCORD_STABLE Rel=ACCORD_RULE Exit=ACCORD_STABLE

//
LUX_REGISTRY: contiguous block of all Data Lux addresses.
//
//
word = base address of block; filled by loader.py at freeze time.
//
//
LUX_REGISTRY_SIZE.word = number of entries.
//
NEW LUX_REGISTRY_BASE
NEW LUX_REGISTRY_SIZE

//
── SCAN_REGISTRY (shared body) ───────────────────────────────
//
//
Internal: RA_TMP2=base, RA_TMP3=size set by wrapper before call.
//
//
IN:  RA_SCAN_BODY.word = body entry; RA_LINK = caller return
//
//
Body ABI: RA_I = current Lux address on entry.
//
//
Normal:     Redi
//
//
Early-exit: Move C_1 → RA_SCAN_STOP; Redi
//
//
NOTE: SAL_REG_END, RA_SAL_CUR, RA_SCAN_STOP are global.
//
//
Callers that invoke nested scans must save/restore these themselves.
//
NEW RA_SCAN_BODY
NEW RA_SCAN_STOP
NEW SAL_REG_END

NOLINK
CLEAR SCAN_REGISTRY El1=RA_SCAN_STOP
NOVA SAL_REGEND Op=Add El1=RA_TMP2 El2=RA_TMP3 Exit=SAL_REG_END
NOVA SAL_INIT Op=Move El1=RA_TMP2 Exit=RA_SAL_CUR
JEQ SAL_LOOP El1=RA_SAL_CUR El2=SAL_REG_END Exit=SAL_DONE
NOVA SAL_READ Op=Read El1=RA_SAL_CUR Exit=RA_I
NOVA SAL_CALL Op=Voca El1=RA_SCAN_BODY Exit=RA_LINK
JZ SAL_STOPCHK El1=RA_SCAN_STOP Exit=SAL_ADVANCE
NOVA SAL_STOPDONE Op=Jump Exit=SAL_DONE
NOVA SAL_ADVANCE Op=Add El1=RA_SAL_CUR El2=C_1 Exit=RA_SAL_CUR
JEQ SAL_NEXTCHK El1=RA_SAL_CUR El2=SAL_REG_END Exit=SAL_DONE
NOVA SAL_LB Op=Jump Exit=SAL_READ
RREDI SAL_DONE
//
── SCAN_ALL_LUX: iterate all Data Lux ────────────────────────
//
//
Sets RA_TMP2=LUX_REGISTRY_BASE, RA_TMP3=LUX_REGISTRY_SIZE,
//
//
then tail-calls SCAN_REGISTRY (RA_LINK restored automatically on return).
//
NOLINK
NOVA SCAN_ALL_LUX Op=Move El1=LUX_REGISTRY_BASE Exit=RA_TMP2
NOVA SAL_LOAD_SZ Op=Move El1=LUX_REGISTRY_SIZE Exit=RA_TMP3
NOVA SAL_TAIL Op=Jump Exit=SCAN_REGISTRY

//
── NOVA_REGISTRY: instruction-only Lux (for EMIT_MISSED) ────
//
NEW NOVA_REGISTRY_BASE
NEW NOVA_REGISTRY_SIZE

//
── SCAN_NOVA_LUX: iterate instruction Lux only ──────────────
//
//
Same pattern: load NOVA_REGISTRY, tail-call SCAN_REGISTRY.
//
NOLINK
NOVA SCAN_NOVA_LUX Op=Move El1=NOVA_REGISTRY_BASE Exit=RA_TMP2
NOVA SIL_LOAD_SZ Op=Move El1=NOVA_REGISTRY_SIZE Exit=RA_TMP3
NOVA SIL_TAIL Op=Jump Exit=SCAN_REGISTRY


//
── INIT_LUX_SCAN ─────────────────────────────────────────────
//
//
OUT: RA_TMP2 = alloc_ptr (total luces used), RA_I = 1. Leaf.
//
//
(Legacy helper — RA_TMP2 is alloc_ptr, not NEXT_ID.)
//
NOLINK
NOVA INIT_LUX_SCAN Op=Read El1=K_CURSOR Exit=RA_TMP2
NOVA ILS_INIT Op=Move El1=C_1 Exit=RA_I
RREDI ILS_RET


//
── SCAN_LUMEN_OF ─────────────────────────────────────────────
//
//
Walks all lumina of RA_SCAN_LUX, calling RA_SCAN_BODY for each.
//
//
Body ABI: RA_TW_LUMEN = current lumen address on entry.
//
//
Body may read Rel via LR (aether[lumen+0]),
//
//
Exit via LT (aether[lumen+1]),
//
//
Next via LX (aether[lumen+2]).
//
//
Normal:     Redi (RA_SCAN_STOP = 0 → continue)
//
//
Early-exit: Move C_1 → RA_SCAN_STOP; Redi
//
//
Non-leaf — return is automatic.
//
NEW RA_SCAN_LUX

NOLINK
CLEAR SCAN_LUMEN_OF El1=RA_SCAN_STOP
LH    SLO_LH        RA_SCAN_LUX RA_TW_LUMEN
//
lumens_head of RA_SCAN_LUX
//
JZ SLO_LOOP El1=RA_SR_REL Exit=SLO_DONE
NOVA SLO_CALL Op=Voca El1=RA_SCAN_BODY Exit=RA_LINK
JZ SLO_STOPCHK El1=RA_SCAN_STOP Exit=SLO_ADVANCE
//
STOP==0 → advance; STOP!=0 → done
//
NOVA SLO_STOPDONE Op=Jump Exit=SLO_DONE
/Inline lumen advance: was LX (8 steps including SR_GLX call).
/Now 3 steps: advance RA_TW_LUMEN by 2, read next rel, loop.
/NOTE: this skips OVERFLOW_REL handling that SR_GLX had. Safe here
/because SCAN_LUMEN_OF is only called on bootstrap rule-lux whose
/lumen capacity is exactly pre-counted by the prepass — no overflow
/occurs in practice. When PLAN_BOUND_NEXT.md is implemented,
/OVERFLOW_REL disappears entirely and this note goes away./
NOVA SLO_ADVANCE Op=Add El1=RA_TW_LUMEN El2=C_2 Exit=RA_TW_LUMEN
NOVA SLO_ADV_REL Op=Read El1=RA_TW_LUMEN Exit=RA_SR_REL
NOVA SLO_LB Op=Jump Exit=SLO_LOOP
RREDI SLO_DONE
//
OUT: RA_TMP = count of adopted accords. Non-leaf.
//
NOLINK
CLEAR ACCORD_CHECK El1=RA_TMP
NOVA AC_SETBODY Op=Move El1=AC_BODY Exit=RA_SCAN_BODY
RVOCA AC_SCANJ Body=SCAN_ALL_LUX

RREDI AC_DONE_r

//
── AC_BODY: per-Lux body for ACCORD_CHECK ────────────────────
//
NOLINK
NOVA AC_BODY Op=Move El1=RA_I Exit=RA_SR_LUX
NOVA AC_SETREL Op=Move El1=ACCORD_USE Exit=RA_SR_REL
RVOCA AC_WOJ Body=SR_WALK_ONE
JZ AC_GOT El1=RA_SR_OUT Exit=AC_RESTORE
NOVA AC_COUNT Op=Add El1=RA_TMP El2=C_1 Exit=RA_TMP
RREDI AC_RESTORE
