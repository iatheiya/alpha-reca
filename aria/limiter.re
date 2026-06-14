============================================================
//limiter.re — Limiter Accord: visibility into ignored content

SCAN_ALL_LUX body convention:
Normal return: Redi (RA_SCAN_STOP = 0, scanner continues)
Early exit:    Move C_1 → RA_SCAN_STOP; Redi (scanner sees stop flag → done)

DEPENDENCY: aspects.re, core/constants.re, runtime/regs.re,
aria/accord.re, aria/symphony.re, aria/output.re//
============================================================

NEWREF LIMITER_ACCORD
LINK LIMITER_ACCORD ACCORD_RULE LIMITER_ACCORD

/K_LIMITER_ADOPTED: 1 if limiter.re was loaded, 0 otherwise.
/Set at freeze time by loader.py — O(1) check in LIMITER_ADOPTED.
NEW K_LIMITER_ADOPTED

NEWSET SF_LIMITER_HDR "[Limiter]\n"
NEWSET SF_LIMITER_BARE "Bare Lux: "


── LIMITER_ADOPTED ───────────────────────────────────────────
/OUT: RA_FLAG = 1 if adopted. O(1) — reads K_LIMITER_ADOPTED flag set at freeze.
NOLINK
ITO LIMITER_ADOPTED Move El1=K_LIMITER_ADOPTED Exit=RA_FLAG
RREDI LA_DONE_r

── LIMITER_CHECK ─────────────────────────────────────────────
/Non-leaf — RA_LINK is saved/restored automatically by the call stack.
NOLINK
ITO LIMITER_CHECK Move    El1=SF_LIMITER_HDR Exit=RA_TW_LUX
RVOCA LIM_HDR_R EMIT_STR_ENTRY
CLEAR LIM_STARTSCAN RA_TMP3
ITO LIM_SETBODY   Move    El1=LIM_BODY  Exit=RA_SCAN_BODY
RVOCA LIM_SCANJ SCAN_ALL_LUX
ITO LIM_REPORT    Move    El1=SF_LIMITER_BARE Exit=RA_TW_LUX
RVOCA LIM_RPT_R EMIT_STR_ENTRY
ITO LIM_RPT_NUM   Move    El1=RA_TMP3   Exit=RA_TMP2
RVOCA LIM_NUM_R EMIT_INT_ENTRY
ITO LIM_RPT_NL    Move    El1=NL_STR Exit=RA_TW_LUX
RVOCA LIM_NL_R EMIT_STR_ENTRY
RREDI LIM_DONE_r

── LIM_BODY ──────────────────────────────────────────────────
/After LH, RA_SR_REL holds the first relation value (0 = no lumens).
NOLINK
LH LIM_BODY RA_I RA_TMP
JZ LIM_HAS_LUM RA_SR_REL LIM_BARE
ITO LIM_HAS_LUMG  Jump      Exit=LIM_BODY_END
ITO LIM_BARE      Read   El1=RA_I   Exit=RA_TMP
JZ LIM_NOT_BARE RA_TMP LIM_CB
ITO LIM_NOT_BAREG Jump      Exit=LIM_BODY_END
ITO LIM_CB        Add       El1=RA_TMP3 El2=C_1   Exit=RA_TMP3
RREDI LIM_BODY_END