//============================================================//
//aria/accord.re — Convention enforcement + universal Lux scanner//

//New layout: SCAN_ALL_LUX reads LUX_REGISTRY (a contiguous block of//
//Data Lux addresses written by loader.py at freeze time).//
//This is pure data — no type tags, no typed regions, just the same//
//information viewed differently.//

//DEPENDENCY: aspects.re, core/constants.re, runtime/layout.re,//
//aria/symphony.re, runtime/registers.re//
//============================================================//

NEW ACCORD_RULE
NEWREF ACCORD_USE
NEW ACCORD_EXCEPT

NEW ACCORD_STABLE
LINK ACCORD_STABLE ACCORD_RULE ACCORD_STABLE

//LUX_REGISTRY: contiguous block of all Data Lux addresses.//
//word = base address of block; filled by loader.py at freeze time.//
//LUX_REGISTRY_SIZE.word = number of entries.//
NEW LUX_REGISTRY_BASE
NEW LUX_REGISTRY_SIZE

//── SCAN_REGISTRY (shared body) ───────────────────────────────//
//Internal: RA_TMP2=base, RA_TMP3=size set by wrapper before call.//
//IN:  RA_SCAN_BODY.word = body entry; RA_LINK = caller return//
//Body ABI: RA_I = current Lux address on entry.//
//Normal:     Redi//
//Early-exit: Move C_1 → RA_SCAN_STOP; Redi//
//NOTE: SAL_REG_END, RA_SAL_CUR, RA_SCAN_STOP are global.//
//Callers that invoke nested scans must save/restore these themselves.//
NEW RA_SCAN_BODY
NEW RA_SCAN_STOP
NEW SAL_REG_END

NOLINK
CLEAR SCAN_REGISTRY RA_SCAN_STOP
ITO SAL_REGEND     Add       El1=RA_TMP2 El2=RA_TMP3 Exit=SAL_REG_END
ITO SAL_INIT       Move      El1=RA_TMP2         Exit=RA_SAL_CUR
JEQ   SAL_LOOP       RA_SAL_CUR SAL_REG_END SAL_DONE
ITO SAL_READ       Read   El1=RA_SAL_CUR      Exit=RA_I
ITO SAL_CALL       Voca      El1=RA_SCAN_BODY   Exit=RA_LINK
JZ    SAL_STOPCHK    RA_SCAN_STOP SAL_ADVANCE
ITO SAL_STOPDONE   Jump      Exit=SAL_DONE
ITO SAL_ADVANCE    Add       El1=RA_SAL_CUR El2=C_1 Exit=RA_SAL_CUR
JEQ   SAL_NEXTCHK    RA_SAL_CUR SAL_REG_END SAL_DONE
ITO SAL_LB         Jump      Exit=SAL_READ
RREDI SAL_DONE
//── SCAN_ALL_LUX: iterate all Data Lux ────────────────────────//
//Sets RA_TMP2=LUX_REGISTRY_BASE, RA_TMP3=LUX_REGISTRY_SIZE,//
//then tail-calls SCAN_REGISTRY (RA_LINK restored automatically on return).//
NOLINK
ITO SCAN_ALL_LUX  Move El1=LUX_REGISTRY_BASE Exit=RA_TMP2
ITO SAL_LOAD_SZ   Move El1=LUX_REGISTRY_SIZE Exit=RA_TMP3
ITO SAL_TAIL      Jump Exit=SCAN_REGISTRY

//── ITO_REGISTRY: instruction-only Lux (for EMIT_MISSED) ────//
NEW ITO_REGISTRY_BASE
NEW ITO_REGISTRY_SIZE

//── SCAN_ITO_LUX: iterate instruction Lux only ──────────────//
//Same pattern: load ITO_REGISTRY, tail-call SCAN_REGISTRY.//
NOLINK
ITO SCAN_ITO_LUX  Move El1=ITO_REGISTRY_BASE Exit=RA_TMP2
ITO SIL_LOAD_SZ     Move El1=ITO_REGISTRY_SIZE Exit=RA_TMP3
ITO SIL_TAIL        Jump Exit=SCAN_REGISTRY


//── INIT_LUX_SCAN ─────────────────────────────────────────────//
//OUT: RA_TMP2 = alloc_ptr (total luces used), RA_I = 1. Leaf.//
//(Legacy helper — RA_TMP2 is alloc_ptr, not NEXT_ID.)//
NOLINK
ITO INIT_LUX_SCAN Read El1=K_CURSOR Exit=RA_TMP2
ITO ILS_INIT      Move    El1=C_1              Exit=RA_I
RREDI ILS_RET


//── SCAN_LUMEN_OF ─────────────────────────────────────────────//
//Walks all lumina of RA_SCAN_LUX, calling RA_SCAN_BODY for each.//
//Body ABI: RA_TW_LUMEN = current lumen address on entry.//
//Body may read Rel via LR (aether[lumen+0]),//
//Exit via LT (aether[lumen+1]),//
//Next via LX (aether[lumen+2]).//
//Normal:     Redi (RA_SCAN_STOP = 0 → continue)//
//Early-exit: Move C_1 → RA_SCAN_STOP; Redi//
//Non-leaf — return is automatic.//
NEW RA_SCAN_LUX

NOLINK
CLEAR SCAN_LUMEN_OF RA_SCAN_STOP
LH    SLO_LH        RA_SCAN_LUX RA_TW_LUMEN       //lumens_head of RA_SCAN_LUX//
JZ SLO_LOOP RA_SR_REL SLO_DONE
ITO SLO_CALL      Voca      El1=RA_SCAN_BODY   Exit=RA_LINK
JZ    SLO_STOPCHK   RA_SCAN_STOP SLO_ADVANCE  //STOP==0 → advance; STOP!=0 → done//
ITO SLO_STOPDONE  Jump      Exit=SLO_DONE
/Inline lumen advance: was LX (8 steps including SR_GLX call).
/Now 3 steps: advance RA_TW_LUMEN by 2, read next rel, loop.
/NOTE: this skips OVERFLOW_REL handling that SR_GLX had. Safe here
/because SCAN_LUMEN_OF is only called on bootstrap rule-lux whose
/lumen capacity is exactly pre-counted by the prepass — no overflow
/occurs in practice. When PLAN_BOUND_NEXT.md is implemented,
/OVERFLOW_REL disappears entirely and this note goes away./
ITO SLO_ADVANCE   Add  El1=RA_TW_LUMEN El2=C_2 Exit=RA_TW_LUMEN
ITO SLO_ADV_REL   Read El1=RA_TW_LUMEN         Exit=RA_SR_REL
ITO SLO_LB        Jump      Exit=SLO_LOOP
RREDI SLO_DONE
//OUT: RA_TMP = count of adopted accords. Non-leaf.//
NOLINK
CLEAR ACCORD_CHECK RA_TMP
ITO AC_SETBODY   Move      El1=AC_BODY     Exit=RA_SCAN_BODY
RVOCA AC_SCANJ SCAN_ALL_LUX

RREDI AC_DONE_r

//── AC_BODY: per-Lux body for ACCORD_CHECK ────────────────────//
NOLINK
ITO AC_BODY     Move      El1=RA_I        Exit=RA_SR_LUX
ITO AC_SETREL   Move      El1=ACCORD_USE  Exit=RA_SR_REL
RVOCA AC_WOJ SR_WALK_ONE
JZ AC_GOT RA_SR_OUT AC_RESTORE
ITO AC_COUNT    Add       El1=RA_TMP El2=C_1 Exit=RA_TMP
RREDI AC_RESTORE