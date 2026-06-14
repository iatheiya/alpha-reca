//aria/iris.re — Execution trace observer ("True Name")

Trace is a PURE OBSERVER: it never modifies the instruction graph.
The interpreter writes word(pc) into the trace buffer on every step
when K_TRACE_POS is nonzero. iris.re only controls the flag
and reads the buffer.

Trace Lux: a named marker. Its ID is used as a sentinel value in
shioreru.re and other places that check whether tracing was active.
It is NEVER used as Op= in any instruction.

DEPENDENCY: aspects.re, core/constants.re, runtime/layout.re,
runtime/regs.re, aria/output.re, aria/ascii.re//

BLOCK IRIS_BUF 4096

NEW RA_IRIS_BUF
NEW RA_IRIS_SIZE
SETREF RA_IRIS_BUF IRIS_BUF
SET    RA_IRIS_SIZE 4096

/Trace: named marker for use as a sentinel/tag. Never used as an opcode.
NEW Trace

NEW ND_DUMP_END

── IRIS_TRACE_START ─────────────────────────────────────────
//Enable tracing: write RA_IRIS_BUF address into K_TRACE_POS.
The interpreter will write word(pc) there on every step.
Leaf. Voca with RVOCA.//
NOLINK
ITO IRIS_TRACE_START Write El1=K_TRACE_POS El2=RA_IRIS_BUF
RREDI NTS_RET

── IRIS_TRACE_STOP ───────────────────────────────────────────
//Disable tracing: write 0 into K_TRACE_POS.
Leaf. Voca with RVOCA.//
NOLINK
ITO IRIS_TRACE_STOP  Write El1=K_TRACE_POS El2=C_0
RREDI NSP_RET

── IRIS_CHECK_FULL ───────────────────────────────────────────
//Stop tracing if buffer is full (pos >= base + size).
Non-leaf — RA_LINK is saved/restored automatically by the call stack.//
NOLINK
ITO IRIS_CHECK_FULL  Add       El1=RA_IRIS_BUF El2=RA_IRIS_SIZE Exit=RA_TMP
ITO NCF_POS          Read   El1=K_TRACE_POS              Exit=RA_TMP2
ITO NCF_CMP          Less El1=RA_TMP2 El2=RA_TMP           Exit=RA_FLAG
ITO NCF_CMPJ         JumpIf    El1=RA_FLAG  Exit=NCF_OK
RVOCA NCF_STOPJ IRIS_TRACE_STOP
RREDI NCF_RESTJ
RREDI NCF_OK

── IRIS_DUMP ─────────────────────────────────────────────────
//Emit the trace buffer as run-length encoded op IDs.
Format: /count.id  (count=1 → /id)
Non-leaf — RA_LINK is saved/restored automatically by the call stack.//
NOLINK
ITO IRIS_DUMP        Read  El1=K_TRACE_POS  Exit=ND_DUMP_END
JEQ ND_EMPTY ND_DUMP_END RA_IRIS_BUF ND_DONE_DIRECT
ITO ND_INIT_I        Move     El1=RA_IRIS_BUF       Exit=RA_TMP
ITO ND_INITCNT       Move     El1=C_1               Exit=RA_TMP3
ITO ND_LOADFST       Read  El1=RA_TMP            Exit=RA_TMP2
ITO ND_INC0          Add      El1=RA_TMP El2=C_1   Exit=RA_TMP

JEQ ND_LOOP RA_TMP ND_DUMP_END ND_FLUSH
ITO ND_LOAD          Read  El1=RA_TMP            Exit=RA_TMP4
JEQ ND_CMP RA_TMP4 RA_TMP2 ND_REPEAT
/NOTE: RCALL_AT is builder-class — unwired until LOAD_MAIN runs (BUGS.md Pattern N).
RCALL_AT ND_DIFF_J ND_EMIT_RUN ND_ADV

ITO ND_REPEAT        Add      El1=RA_TMP3 El2=C_1  Exit=RA_TMP3
ITO ND_ADV           Add      El1=RA_TMP El2=C_1   Exit=RA_TMP
ITO ND_ADVLB         Jump     Exit=ND_LOOP

RCALL_AT ND_FLUSH ND_EMIT_RUN ND_DONE

RREDI ND_DONE
RREDI ND_DONE_DIRECT

── ND_EMIT_RUN ───────────────────────────────────────────────/
//Emit one run: /count.id  (if count==1: /id)
Non-leaf — RA_LINK is saved/restored automatically by the call stack.//
NOLINK
ITO ND_EMIT_RUN      Move     El1=RA_TMP2  Exit=RA_I

ITO ND_ER_SLASH      Move     El1=SLASH    Exit=RA_BYTE
RVOCA ND_ER_SLJ PUT_BYTE

JEQ ND_ER_CHK RA_TMP3 C_1 ND_ER_ID

ITO ND_ER_CNTL       Move     El1=RA_TMP3  Exit=RA_TMP2
RVOCA ND_ER_CNTJ EMIT_INT_ENTRY

ITO ND_ER_DOT        Move     El1=DOT      Exit=RA_BYTE
RVOCA ND_ER_DOTJ PUT_BYTE

ITO ND_ER_ID         Move     El1=RA_I     Exit=RA_TMP2
RVOCA ND_ER_IDJ EMIT_INT_ENTRY

ITO ND_ER_IDRET      Move     El1=C_1      Exit=RA_TMP3
ITO ND_ER_LNX        Read  El1=RA_TMP   Exit=RA_TMP2
RREDI ND_ER_RETJ