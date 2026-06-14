============================================================
//shioreru.re — Shioreru: execution report written to stderr

ROLE: Shioreru / Explain After.
Called on fault (via FAULT_VECTOR) or on normal exit.
Writes an execution summary to stderr by temporarily switching
the OB sink (RA_OB_FD) from STDOUT to STDERR. After the report
it restores the previous sink so subsequent OB writes (if any)
go where they used to.

── ONE PIPELINE FOR ALL OUTPUT ───────────────────────────────
There is only one set of byte-emitting subroutines in the system:
PUT_BYTE / EMIT_STR_ENTRY / EMIT_INT_ENTRY / FLUSH  (output.re)
Where the bytes go is decided by RA_OB_FD. Shioreru does not have
its own writer family — it just changes the destination, calls
the same EMIT_* routines, and FLUSHes at boundaries. This is the
"one universal mechanism" stance: a second sink is a parameter,
not a second implementation.

FLOW (SHIORERU_REPORT / EXHALE_REPORT):
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
EXH_SAVED_FD  — saved RA_OB_FD       (private to shioreru.re)

All cross-step return targets reference the next ITO Lux directly:
ITO is self-referential (word = own lid), so
Move El1=NEXT_LABEL Exit=RA_LINK
Jump Exit=SUB
places NEXT_LABEL's lid into RA_LINK.word, and SUB's terminating
JumpReg RA_LINK jumps there. No forward NEW + SETREF wrapper needed.

DEPENDENCY: aspects.re,
core/constants.re (C_0, C_1),
aria/io.re (STDERR),
runtime/regs.re (RA_LINK, RA_TW_LUX, RA_TMP, RA_TMP2, RA_FLAG),
aria/output.re (EMIT_STR_ENTRY, EMIT_INT_ENTRY, FLUSH, RA_OB_FD),
runtime/layout.re (K_TRACE_POS),
aria/accord.re (ACCORD_CHECK),
aria/iris.re   (RA_IRIS_BUF),
aria/limiter.re (LIMITER_ADOPTED, LIMITER_CHECK)//
============================================================


── Strings ───────────────────────────────────────────────────
NEWSET SF_EXHALE_HDR "[Exhale]\n"
NEWSET SF_EXHALE_TRACE_ON "Trace: "
NEWSET SF_EXHALE_TRACE_OFF "Trace: off\n"
NEWSET SF_EXHALE_OPS " ops\n"
NEWSET SF_EXHALE_ACC "Accords: "

── Private save slots ────────────────────────────────────────
NEW EXH_SAVED_FD


═══════════════════════════════════════════════════════════════
/EXHALE_REPORT
═══════════════════════════════════════════════════════════════
NOLINK
ITO EXHALE_REPORT  Move El1=RA_OB_FD   Exit=EXH_SAVED_FD

/(3) drain pending output
RVOCA EXH_FL1_R FLUSH

/(4) switch sink to STDERR
ITO EXH_AFTER_FLUSH_OLD Move El1=STDERR Exit=RA_OB_FD

/(5) emit "[Exhale]\n"
ITO EXH_HDR        Move El1=SF_EXHALE_HDR     Exit=RA_TW_LUX
RVOCA EXH_HDR_R EMIT_STR_ENTRY

//(6) Name trace branch
K_TRACE_POS.word = address of the trace-position lux.
Aether[that lux] = 0 when tracing is off, else the write pointer.//
ITO EXH_HDR_AFTER  Read El1=K_TRACE_POS Exit=RA_TMP
JZ    EXH_TRK        RA_TMP EXH_TRACE_OFF
/fall through: tracing is on

ITO EXH_TRACE_ON_HDR    Move El1=SF_EXHALE_TRACE_ON Exit=RA_TW_LUX
RVOCA EXH_TRACE_ON_HDR_R EMIT_STR_ENTRY

ITO EXH_AFTER_TRON_HDR  Sub  El1=RA_TMP El2=RA_IRIS_BUF Exit=RA_TMP2
RVOCA EXH_TA_NUM_R EMIT_INT_ENTRY

ITO EXH_AFTER_TA_NUM    Move El1=SF_EXHALE_OPS    Exit=RA_TW_LUX
RCALL_AT EXH_TA_OPS_R EMIT_STR_ENTRY EXH_STEP4

/trace OFF branch: writes "Trace: off\n", lands on EXH_STEP4
ITO EXH_TRACE_OFF       Move El1=SF_EXHALE_TRACE_OFF Exit=RA_TW_LUX
RVOCA EXH_TRACE_OFF_R EMIT_STR_ENTRY

/(7) ACCORD_CHECK; emit "Accords: <count>\n"
RVOCA EXH_STEP4 ACCORD_CHECK

ITO EXH_AFTER_AC        Move El1=RA_TMP Exit=RA_TMP2
ITO EXH_ACC_HDR         Move El1=SF_EXHALE_ACC Exit=RA_TW_LUX
RVOCA EXH_ACC_HDR_R EMIT_STR_ENTRY

/EMIT_STR does not clobber RA_TMP2 — feed it directly to EMIT_INT
RVOCA EXH_AFTER_ACC_HDR EMIT_INT_ENTRY

ITO EXH_AFTER_ACC_NUM   Move El1=NL_STR Exit=RA_TW_LUX
RVOCA EXH_ACC_NL_R EMIT_STR_ENTRY

/(8) conditionally call LIMITER_CHECK
RVOCA EXH_STEP5 LIMITER_ADOPTED

ITO EXH_AFTER_LA        JumpIf El1=RA_FLAG Exit=EXH_CALL_LIM
ITO EXH_SKIP_LIM        Jump   Exit=EXH_DRAIN

RVOCA EXH_CALL_LIM LIMITER_CHECK

/(9) drain stderr; restore sink and outer RA_LINK; return
RVOCA EXH_DRAIN FLUSH

ITO EXH_AFTER_DRAIN     Move El1=EXH_SAVED_FD  Exit=RA_OB_FD
RREDI EXH_RETJ_r


── Fault handler ─────────────────────────────────────────────
//FAULT_VECTOR.word = ETHER_ENTRY addr, set via SETREF (runs in Wave-B after RVOCA).
On unknown op_id interpreter jumps to ETHER_ENTRY which calls EXHALE_REPORT then halts.//
NEW FAULT_VECTOR
SETREF FAULT_VECTOR ETHER_ENTRY

NOLINK
RVOCA ETHER_ENTRY EXHALE_REPORT
ITO ETH_HALT       End