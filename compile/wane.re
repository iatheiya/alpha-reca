============================================================
//
wane.re — Wane: execution report written to stderr

ROLE: Wane / Explain After.
Called on fault (via FAULT_VECTOR) or on normal exit.
Writes an execution summary to stderr by temporarily switching
the OB sink (RA_OB_FD) from STDOUT to STDERR. After the report
it restores the previous sink so subsequent OB writes (if any)
go where they used to.

── ONE PIPELINE FOR ALL OUTPUT ───────────────────────────────
There is only one set of byte-emitting subroutines in the system:
PUT_BYTE / EMIT_STR_ENTRY / EMIT_INT_ENTRY / FLUSH  (output.re)
Where the bytes go is decided by RA_OB_FD. Wane does not have
its own writer family — it just changes the destination, calls
the same EMIT_* routines, and FLUSHes at boundaries. This is the
"one universal mechanism" stance: a second sink is a parameter,
not a second implementation.

FLOW (WANE_REPORT / EXHALE_REPORT):
1. RA_LINK saved automatically by the call stack (Voca on entry)
2. save current RA_OB_FD  (→ EXH_SAVED_FD)
3. drain pending output to its current FD  (FLUSH)
4. switch RA_OB_FD → STDERR
5. emit "[Exhale]\n"
6. report Name trace ("Trace: <count> ops\n" or "Trace: off\n")
7. ACCORD_CHECK; emit "Accords: <count>\n"
8. if LIMITER_ACCORD adopted → call LIMITER_CHECK
9. drain stderr; restore RA_OB_FD; restore RA_LINK; return

REGISTER DISCIPLINE:
RA_LINK saved/restored automatically by the call stack (Voca/Redi)
EXH_SAVED_FD  — saved RA_OB_FD       (private to wane.re)

All cross-step return targets reference the next NOVA Lux directly:
"NOVA" is self-referential (word = own lid), so
Move El1=NEXT_LABEL Exit=RA_LINK
Jump Exit=SUB
places NEXT_LABEL's lid into RA_LINK.word, and SUB's terminating
JumpReg RA_LINK jumps there. No forward NEW + SEAL wrapper needed.

DEPENDENCY: aspects.re,
core/constants.re (C_0, C_1),
aria/io.re (STDERR),
runtime/registers.re (RA_LINK, RA_TW_LUX, RA_TMP, RA_TMP2, RA_FLAG),
aria/output.re (EMIT_STR_ENTRY, EMIT_INT_ENTRY, FLUSH, RA_OB_FD),
runtime/layout.re (K_TRACE_POS),
aria/accord.re (ACCORD_CHECK),
aria/iris.re   (IRIS_BUF),
aria/limiter.re (LIMITER_ADOPTED, LIMITER_CHECK)
//
============================================================


── Strings ───────────────────────────────────────────────────
SET SF_EXHALE_HDR Value="[Exhale]\n"
SET SF_EXHALE_TRACE_ON Value="Trace: "
SET SF_EXHALE_TRACE_OFF Value="Trace: off\n"
SET SF_EXHALE_OPS Value=" ops\n"
SET SF_EXHALE_ACC Value="Accords: "

── Private save slots ────────────────────────────────────────
NEW EXH_SAVED_FD


═══════════════════════════════════════════════════════════════
/EXHALE_REPORT
═══════════════════════════════════════════════════════════════
NOLINK
NOVA EXHALE_REPORT Op=Move El1=RA_OB_FD Exit=EXH_SAVED_FD

/(3) drain pending output
RVOCA EXH_FL1_R Body=FLUSH

/(4) switch sink to STDERR
NOVA EXH_AFTER_FLUSH_OLD Op=Move El1=STDERR Exit=RA_OB_FD

/(5) emit "[Exhale]\n"
NOVA EXH_HDR Op=Move El1=SF_EXHALE_HDR Exit=RA_TW_LUX
RVOCA EXH_HDR_R Body=EMIT_STR_ENTRY

//
(6) Name trace branch
K_TRACE_POS.word = address of the trace-position lux.
Aether[that lux] = 0 when tracing is off, else the write pointer.
//
NOVA EXH_HDR_AFTER Op=Read El1=K_TRACE_POS Exit=RA_TMP
JZ EXH_TRK El1=RA_TMP Exit=EXH_TRACE_OFF
/fall through: tracing is on

NOVA EXH_TRACE_ON_HDR Op=Move El1=SF_EXHALE_TRACE_ON Exit=RA_TW_LUX
RVOCA EXH_TRACE_ON_HDR_R Body=EMIT_STR_ENTRY

NOVA EXH_AFTER_TRON_HDR Op=Sub El1=RA_TMP El2=IRIS_BUF Exit=RA_TMP2
RVOCA EXH_TA_NUM_R Body=EMIT_INT_ENTRY

NOVA EXH_AFTER_TA_NUM Op=Move El1=SF_EXHALE_OPS Exit=RA_TW_LUX
RCALL_AT EXH_TA_OPS_R EMIT_STR_ENTRY EXH_STEP4

/trace OFF branch: writes "Trace: off\n", lands on EXH_STEP4
NOVA EXH_TRACE_OFF Op=Move El1=SF_EXHALE_TRACE_OFF Exit=RA_TW_LUX
RVOCA EXH_TRACE_OFF_R Body=EMIT_STR_ENTRY

/(7) ACCORD_CHECK; emit "Accords: <count>\n"
RVOCA EXH_STEP4 Body=ACCORD_CHECK

NOVA EXH_AFTER_AC Op=Move El1=RA_TMP Exit=RA_TMP2
NOVA EXH_ACC_HDR Op=Move El1=SF_EXHALE_ACC Exit=RA_TW_LUX
RVOCA EXH_ACC_HDR_R Body=EMIT_STR_ENTRY

/EMIT_STR does not clobber RA_TMP2 — feed it directly to EMIT_INT
RVOCA EXH_AFTER_ACC_HDR Body=EMIT_INT_ENTRY

NOVA EXH_AFTER_ACC_NUM Op=Move El1=NL_STR Exit=RA_TW_LUX
RVOCA EXH_ACC_NL_R Body=EMIT_STR_ENTRY

/(8) conditionally call LIMITER_CHECK
RVOCA EXH_STEP5 Body=LIMITER_ADOPTED

NOVA EXH_AFTER_LA Op=JumpIf El1=RA_FLAG Exit=EXH_CALL_LIM
NOVA EXH_SKIP_LIM Op=Jump Exit=EXH_DRAIN

RVOCA EXH_CALL_LIM Body=LIMITER_CHECK

/(9) drain stderr; restore sink and outer RA_LINK; return
RVOCA EXH_DRAIN Body=FLUSH

NOVA EXH_AFTER_DRAIN Op=Move El1=EXH_SAVED_FD Exit=RA_OB_FD
RREDI EXH_RETJ_r


── Fault handler ─────────────────────────────────────────────
//
FAULT_VECTOR.word = ETHER_ENTRY addr, set via SEAL (runs in Wave-B after RVOCA).
On unknown op_id interpreter jumps to ETHER_ENTRY which calls EXHALE_REPORT then halts.
//
NEW FAULT_VECTOR
SET FAULT_VECTOR Value=ETHER_ENTRY

NOLINK
RVOCA ETHER_ENTRY Body=EXHALE_REPORT
NOVA ETH_HALT Op=End
