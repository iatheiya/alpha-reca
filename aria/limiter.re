============================================================
//
limiter.re — Limiter Accord: visibility into ignored content

SCAN_ALL_LUX body convention:
Normal return: Redi (RA_SCAN_STOP = 0, scanner continues)
Early exit:    Move C_1 → RA_SCAN_STOP; Redi (scanner sees stop flag → done)

DEPENDENCY: aspects.re, core/constants.re, runtime/registers.re,
aria/accord.re, aria/lumen.re, aria/output.re
//
============================================================

SET LIMITER_ACCORD
LINK LIMITER_ACCORD Rel=ACCORD_RULE Exit=LIMITER_ACCORD

/K_LIMITER_ADOPTED: 1 if limiter.re was loaded, 0 otherwise.
/Set at freeze time by loader.py — O(1) check in LIMITER_ADOPTED.
NEW K_LIMITER_ADOPTED  /K_ prefix auto-carries SAVE+LOCK
NEW RA_LIM_COUNT  /bare-lux counter for LIMITER_CHECK's scan callback --
                          /dedicated (not RA_TMP3) because RA_TMP3 is VOLATILE
                          /SCRATCH (registers.re) and VS_TEST_SET clobbers it
                          /on every visited-bit check SCAN_ALL_LUX makes during
                          /the same scan -- using it as a running counter across
                          /scan callbacks silently corrupted the count. Same
                          /reason RA_SAL_CUR exists as SCAN_ALL_LUX's own
                          /dedicated cursor instead of reusing volatile scratch.

SET SF_LIMITER_HDR Value="[Limiter]\n"
SET SF_LIMITER_BARE Value="Bare Lux: "


── LIMITER_ADOPTED ───────────────────────────────────────────
/OUT: RA_FLAG = 1 if adopted. O(1) — reads K_LIMITER_ADOPTED flag set at freeze.
NOLINK
NOVA LIMITER_ADOPTED Op=Move El1=K_LIMITER_ADOPTED Exit=RA_FLAG
RREDI LA_DONE_r

── LIMITER_CHECK ─────────────────────────────────────────────
/Non-leaf — RA_LINK is saved/restored automatically by the call stack.
NOVA LIMITER_CHECK Op=Move El1=SF_LIMITER_HDR Exit=RA_TW_LUX
RVOCA LIM_HDR_R Body=EMIT_STR_ENTRY
NOVA Op=Move El1=C_0 Exit=RA_LIM_COUNT
NOVA Op=Move El1=LIM_BODY Exit=RA_SCAN_BODY
RVOCA LIM_SCANJ Body=SCAN_ALL_LUX
NOVA Op=Move El1=SF_LIMITER_BARE Exit=RA_TW_LUX
RVOCA LIM_RPT_R Body=EMIT_STR_ENTRY
NOVA Op=Move El1=RA_LIM_COUNT Exit=RA_TMP2
RVOCA LIM_NUM_R Body=EMIT_INT_ENTRY
NOVA Op=Move El1=NL_STR Exit=RA_TW_LUX
RVOCA LIM_NL_R Body=EMIT_STR_ENTRY

RREDI LIM_DONE_r

── LIM_BODY ──────────────────────────────────────────────────
/After LH, RA_SR_REL holds the first relation value (0 = no lumina).
NOLINK
LH LIM_BODY RA_I RA_TMP
JZ LIM_HAS_LUM El1=RA_SR_REL Exit=LIM_BARE
NOVA LIM_HAS_LUMG Op=Jump Exit=LIM_BODY_END
NOVA LIM_BARE Op=Read El1=RA_I Exit=RA_TMP
JZ LIM_NOT_BARE El1=RA_TMP Exit=LIM_CB
NOVA LIM_NOT_BAREG Op=Jump Exit=LIM_BODY_END
NOVA LIM_CB Op=Add El1=RA_LIM_COUNT El2=C_1 Exit=RA_LIM_COUNT
RREDI LIM_BODY_END
