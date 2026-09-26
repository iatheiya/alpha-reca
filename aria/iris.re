//
aria/iris.re — Execution trace observer ("True Name")

Trace is a PURE OBSERVER: it never modifies the instruction graph.
The interpreter writes word(pc) into the trace buffer on every step
when K_TRACE_POS is nonzero. iris.re only controls the flag
and reads the buffer.

Trace Lux: a named marker. Its ID is used as a sentinel value in
wane.re and other places that check whether tracing was active.
It is NEVER used as Op= in any instruction.

DEPENDENCY: aspects.re, core/constants.re, runtime/layout.re,
runtime/registers.re, aria/output.re, aria/ascii.re
//

NEW IRIS_BUF  Count=4096

NEW IRIS_BUF In=OWN
NEW RA_IRIS_SIZE
SET RA_IRIS_SIZE Value=4096

/Trace: named marker for use as a sentinel/tag. Never used as an opcode.
NEW Trace

NEW ND_DUMP_END

── IRIS_TRACE_START ─────────────────────────────────────────
//
Enable tracing: write IRIS_BUF address into K_TRACE_POS.
The interpreter will write word(pc) there on every step.
Leaf. Voca with RVOCA.
//
NOLINK
NOVA IRIS_TRACE_START Op=Key El1=K_TRACE_POS El2=IRIS_BUF
RREDI NTS_RET

── IRIS_TRACE_STOP ───────────────────────────────────────────
//
Disable tracing: write 0 into K_TRACE_POS.
Leaf. Voca with RVOCA.
//
NOLINK
NOVA IRIS_TRACE_STOP Op=Key El1=K_TRACE_POS El2=C_0
RREDI NSP_RET

── IRIS_CHECK_FULL ───────────────────────────────────────────
//
Stop tracing if buffer is full (pos >= base + size).
Non-leaf — RA_LINK is saved/restored automatically by the call stack.
//
NOLINK
NOVA IRIS_CHECK_FULL Op=Add El1=IRIS_BUF El2=RA_IRIS_SIZE Exit=RA_TMP
NOVA NCF_POS Op=Read El1=K_TRACE_POS Exit=RA_TMP2
NOVA NCF_CMP Op=Less El1=RA_TMP2 El2=RA_TMP Exit=RA_FLAG
NOVA NCF_CMPJ Op=JumpIf El1=RA_FLAG Exit=NCF_OK
RVOCA NCF_STOPJ Body=IRIS_TRACE_STOP
RREDI NCF_RESTJ
RREDI NCF_OK

── IRIS_DUMP ─────────────────────────────────────────────────
//
Emit the trace buffer as run-length encoded op IDs.
Format: /count.id  (count=1 → /id)
Non-leaf — RA_LINK is saved/restored automatically by the call stack.
//
NOLINK
NOVA IRIS_DUMP Op=Read El1=K_TRACE_POS Exit=ND_DUMP_END
JEQ ND_EMPTY El1=ND_DUMP_END El2=IRIS_BUF Exit=ND_DONE_DIRECT
NOVA ND_INIT_I Op=Move El1=IRIS_BUF Exit=RA_TMP
NOVA ND_INITCNT Op=Move El1=C_1 Exit=RA_TMP3
NOVA ND_LOADFST Op=Read El1=RA_TMP Exit=RA_TMP2
NOVA ND_INC0 Op=Add El1=RA_TMP El2=C_1 Exit=RA_TMP

JEQ ND_LOOP El1=RA_TMP El2=ND_DUMP_END Exit=ND_FLUSH
NOVA ND_LOAD Op=Read El1=RA_TMP Exit=RA_TMP4
JEQ ND_CMP El1=RA_TMP4 El2=RA_TMP2 Exit=ND_REPEAT
/NOTE: RCALL_AT is builder-class — unwired until LOAD_MAIN runs (BUGS.md Pattern N).
RCALL_AT ND_DIFF_J ND_EMIT_RUN ND_ADV

NOVA ND_REPEAT Op=Add El1=RA_TMP3 El2=C_1 Exit=RA_TMP3
NOVA ND_ADV Op=Add El1=RA_TMP El2=C_1 Exit=RA_TMP
NOVA ND_ADVLB Op=Jump Exit=ND_LOOP

RCALL_AT ND_FLUSH ND_EMIT_RUN ND_DONE

RREDI ND_DONE
RREDI ND_DONE_DIRECT

── ND_EMIT_RUN ───────────────────────────────────────────────/
//
Emit one run: /count.id  (if count==1: /id)
Non-leaf — RA_LINK is saved/restored automatically by the call stack.
//
NOLINK
NOVA ND_EMIT_RUN Op=Move El1=RA_TMP2 Exit=RA_I

NOVA ND_ER_SLASH Op=Move El1=SLASH Exit=RA_BYTE
RVOCA ND_ER_SLJ Body=PUT_BYTE

JEQ ND_ER_CHK El1=RA_TMP3 El2=C_1 Exit=ND_ER_ID

NOVA ND_ER_CNTL Op=Move El1=RA_TMP3 Exit=RA_TMP2
RVOCA ND_ER_CNTJ Body=EMIT_INT_ENTRY

NOVA ND_ER_DOT Op=Move El1=DOT Exit=RA_BYTE
RVOCA ND_ER_DOTJ Body=PUT_BYTE

NOVA ND_ER_ID Op=Move El1=RA_I Exit=RA_TMP2
RVOCA ND_ER_IDJ Body=EMIT_INT_ENTRY

NOVA ND_ER_IDRET Op=Move El1=C_1 Exit=RA_TMP3
NOVA ND_ER_LNX Op=Read El1=RA_TMP Exit=RA_TMP2
RREDI ND_ER_RETJ
