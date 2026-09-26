//
============================================================
parser.re — Reca runtime .re parser  (PS_MAIN entry point)

ARCHITECTURE FIX: All JumpIf and Jump luxes are now NOVA-based.
The old pattern (LINK X Op JumpIf + LINK PREV Next X at bottom)
was broken because _args() uses "last-overwrite wins" (oldest
lumen), so the explicit LINK overrides were always dead.  The
auto-Next lumen (set when the following NOVA was processed) is
the OLDEST and always wins.  NOVA-based JumpIf gets its Next via
auto-link to the following instruction, which is correct.

REMOVED: PS_LK_BQ / PS_LK_VS fast-paths.  PS_BQ_BASE=0 and
PS_VS_BASE=0 at load time → those paths always computed the wrong
Lux ID.  General BS_HTAB lookup handles any remaining names.

WHY THIS FILE EXISTS ALONGSIDE wax.re (both parse the same .re
grammar — NEW/SET/NOVA/LINK/etc — and this is NOT accidental
duplication):
wax.re's LOAD_MAIN owns the wave pipeline (file iteration, lcount,
declare/wire bookkeeping) for the one, permanent program — it is
also the target for the eventual M1 native-binary goal: once Yaku
can compile the whole codebase (including LOAD_MAIN itself) to a
standalone ARM64 binary, that binary still needs a way to read .re
source with zero Python anywhere in the loop. parser.re's PS_MAIN is
the grammar-level line/token parser wax.re calls into per record
during that same load-time prepass — NOT a separate hot-load-into-
an-already-running-program mechanism (an earlier version of this
file was designed around that idea; it never had a real caller for
it and the distinction does not hold up). One pipeline, one moment
in time (load time), two files because wax.re owns wave orchestration
and parser.re owns grammar parsing — not because they run at
different points in a program's lifecycle.
Because of this, PS_LK/PS_INS resolve and declare names directly
against the one real, shared BS_HTAB (intern.re) — the same table
wax.re's own BS_INTERN/LOAD_INTERN use. There is no second, LSYM-
style table: an earlier design gave parser.re its own private 512-
slot symbol table on the theory that hot-loaded-module names needed
isolation from the permanent program's names. That scenario doesn't
exist now that this file is understood as load-time-only, so the
isolation it justified was removed along with it — a name parser.re
declares IS a permanent program name, same as one wax.re declares.
Shared low-level mechanics (HT_INSERT/HT_LOOKUP, the open-addressing
algorithm itself) are reused, not reimplemented, same as before.

DEPENDS ON: aspects.re, core/constants.re, aria/ascii.re,
aria/math.re, aria/io.re, runtime/registers.re,
target/linux_generic.re, aria/output.re
//
============================================================

── Parser state registers ────────────────────────────────────
NEW PR_SAVED_READFD  /saved outer fd for nested LOAD
NEW PR_LOAD_FD       /fd returned by openat
NEW PR_SYM           /symbol lookup result (Lux ID, 0=not found)
NEW PR_EL1          /element 1 (src/name)
NEW PR_EL2          /element 2 (rel/key)
NEW PR_EL3          /element 3 (exit_lux/val)
NEW PR_D3_OFF        /3-digit parse: offset into token buffer
NEW PR_D3_VAL        /3-digit parse: result
NEW PR_TMP           /scratch 1
NEW PR_TMP2          /scratch 2
NEW PR_TMP3          /scratch 3
NEW PR_TMP4          /scratch 4

//
── Auto-Next state ──────────────────────────────────────────
PR_LAST_INSTR: ID of last instruction Lux created.
0 = no previous instruction (NOLINK state).
"Set" to PR_AN_LAST after each instruction-creating command.
PR_AN_FIRST: first Lux of the current instruction sequence
(receives incoming Next lumen from PR_LAST_INSTR).
PR_AN_LAST: last Lux of the current instruction sequence
(stored into PR_LAST_INSTR for the next command).
All instruction-creating handlers set PR_AN_FIRST + PR_AN_LAST
then Jump → PS_INSTR_DONE (not PS_LINE_DONE directly).
"NOLINK" sets PR_LAST_INSTR = 0 and Jumps → PS_LINE_DONE directly.
//

NEW PR_LAST_INSTR
NEW PR_AN_FIRST
NEW PR_AN_LAST

── Return-address holders (word set via SEAL at bottom) ──────

── Per-subroutine return registers ───────────────────────────


── Constants ─────────────────────────────────────────────────
//
AT_FDCWD, O_RDONLY, SYS_CLOSE are shared system constants and live in
aria/io.re and target/linux_generic.re. parser.re uses them
directly — no PS_-prefixed shadow copies.
//
============================================================/
//
PS_NT — scan next token: now just BS_READ_TOKEN (lexer.re). PS_RB
(byte read) and PS_RL (line buffering) are gone entirely -- both were
private reimplementations of BS_READ_BYTE and a materialized line
buffer neither PS_ST nor PGB_PLACEHOLDER (the two callers that looked
like they needed positional access into a buffered line) actually
required: both are purely sequential forward scans, streamed via
BS_READ_BYTE directly, with no buffer at all.
//
============================================================


/PS_INT (decimal integer parser) removed with SET's alignment -- SET was
/its only caller, and SET now takes its integer value from the pre-read
/(SK_ARG_Value, resolved to a constant cell) instead of parsing a token.


============================================================
//
PS_INS — insert (RA_LOAD_HASH, PR_EL1=lux_id) into BS_HTAB via BS_HT_REGISTER
BUG FIX: this function previously had no RREDI of its own — it fell
through (fall-through next=0) directly into whatever function happened
to be allocated immediately after it in memory (PS_CMD_NEW, by accident
of file order), silently skipping the real return to PS_INS's actual
caller. The old comment ("Returns via JumpReg PR_INS_RET") described a
register-based return mechanism that was never implemented anywhere —
PR_INS_RET does not exist in this file. Standardized to the same
RVOCA/RREDI convention every other function here already uses.
Formerly inserted into a private LSYM table (PR_LSYM, 512 slots) --
see the file header for why that table is gone. A name parser.re
declares now becomes a real, permanent BS_HTAB entry, same as one
wax.re declares via LOAD_CMD_NEW.
Its own six-line HT_INSERT field setup (RA_HT_HASH/BASE/MASK/SIZE/
NAME/NLEN) used to be written out here by hand, byte-for-byte the same
as BS_INTERN_AT's own (intern.re) -- both target the one shared
BS_HTAB. Now shares BS_HT_REGISTER (intern.re) instead: PS_INS's only
remaining job is to say which address to register (PR_EL1).
//
============================================================
NOVA PS_INS Op=Move El1=PR_EL1 Exit=RA_HT_LID
RVOCA PS_INS_RJ Body=BS_HT_REGISTER
RREDI PS_INS_DONE

============================================================
/LOAD_ALLOC_SIZED — shared sized-allocation helper
IN:  SK_TMP = lcount (0 if none -- LOAD_LCOUNT_GET's own convention).
     RA_ALLOC_BASE = base slot count for the lux kind being sized (C_1
     for a plain data lux, NOVA_SIZE for a NOVA lux). RA_ALLOC_TERM_FORCE
     = 1 if the +1 terminator slot must always be added (data: the base
     slot has no built-in pad of its own) or 0 if it is only needed when
     lcount>0 (nova: slot 6 already serves as an implicit terminator
     when there is nothing else to hold).
OUT: RA_ALLOC_RESULT = base address (via ALLOC).
When RA_ALLOC_BASE==NOVA_SIZE, registers the result in SK_NOVA_ADDRS
(Python parity: _alloc_nova → _nova_addrs_set.add).
Non-leaf (calls ALLOC).
============================================================
NOVA LOAD_ALLOC_SIZED Op=Mul El1=SK_TMP El2=C_2 Exit=SK_FLAG
NOVA LOAD_AS_BASE Op=Add El1=RA_ALLOC_BASE El2=SK_FLAG Exit=RA_ALLOC_COUNT
JZ LOAD_AS_TERMCK El1=RA_ALLOC_TERM_FORCE Exit=LOAD_AS_CONDCK
NOVA LOAD_AS_FORCEADD Op=Add El1=RA_ALLOC_COUNT El2=C_1 Exit=RA_ALLOC_COUNT
NOVA LOAD_AS_FORCEJ Op=Jump Exit=LOAD_AS_DOALLOC
NOLINK
JZ LOAD_AS_CONDCK El1=SK_TMP Exit=LOAD_AS_DOALLOC
NOVA LOAD_AS_CONDADD Op=Add El1=RA_ALLOC_COUNT El2=C_1 Exit=RA_ALLOC_COUNT
RVOCA LOAD_AS_DOALLOC Body=ALLOC
JEQ LOAD_AS_NACK El1=RA_ALLOC_BASE El2=NOVA_SIZE Exit=LOAD_AS_NAREG
RREDI RET_LOAD_ALLOC_SIZED
NOLINK
NOVA LOAD_AS_NAREG Op=Write El1=SK_NOVA_ADDRS_PTR El2=RA_ALLOC_RESULT
NOVA Op=Add El1=SK_NOVA_ADDRS_PTR El2=C_1 Exit=SK_NOVA_ADDRS_PTR
NOVA Op=Write El1=SK_NOVA_ADDRS_PTR El2=C_0
RREDI RET_LOAD_AS_NAREG





/PS_NEXT_SYMBOL (read-token + PS_LK) removed with LINK's alignment --
/it was LINK's private self-reader for the rel/exit operands, and LINK
/now takes those from the pre-read (MA1/MA2). No other caller remained.


── Parameter registers this session emptied out ────────────────
//
Every PS_MK_*_PATTERN in this file is gone now (JCQ/EMIT/LOP/WO/GL/
RCALL) -- each turned out to have a real synth.re counterpart, calling
it directly instead of building an independent copy (including LH,
despite an earlier note here claiming otherwise; it turned out grep
had just missed WAX_CMD Name=LH SYNTH_LH. See BUGS.md/HISTORY.md for
the full review). Their own PR_EMP_*/PR_CAL_LANDING/PR_LOP_LUMENOP/
PR_JCQ_B2/PR_GL_SRFUNC/PR_GL_NAMETGT/PR_MAC_NM_R parameter registers
are gone with them.
//
/PR_JT_BASE_REG/DEF_REG/PSI_OUTER removed — PS_INIT_JT128 now uses RA_TMP2/RA_TMP3 directly (leaf)



── PS_INSTR_DONE: universal exit for instruction-creating commands ─
//
Wires prev[SLOT_NEXT=5] = PR_AN_FIRST to auto-link sequential instructions.
IN:  PR_AN_FIRST = first Lux of current sequence (gets incoming nxt link)
PR_AN_LAST  = last Lux of current sequence (stored as PR_LAST_INSTR)
Flow:
if PR_LAST_INSTR == 0: skip link, just update tracker
if PR_LAST_INSTR != 0: write aether[prev + SLOT_NEXT] = PR_AN_FIRST, then update
//
NOLINK
JZ PS_INSTR_DONE El1=PR_LAST_INSTR Exit=PID_DO_UPDATE
/PR_LAST_INSTR != 0: write next-link into slot 5 of previous instruction
NOVA PID_LINK_A Op=Add El1=PR_LAST_INSTR El2=C_5 Exit=RA_TMP /addr+SLOT_NEXT
NOVA PID_LINK_B Op=Write El1=RA_TMP El2=PR_AN_FIRST /prev[5] = first
/Update tracker: PR_LAST_INSTR = PR_AN_LAST (last lux of this sequence)
NOVA PID_DO_UPDATE Op=Move El1=PR_AN_LAST Exit=PR_LAST_INSTR
RREDI PID_RET_r





/PS_ALLOC_SUBLUX removed: its callers (PS_MK_*_PATTERN) are all gone,
/replaced by compile-time NOVAE in synth.re. Runtime NOVA allocation
/is no longer needed.


/PS_DEF_BUILD_BODY / PS_DEF_BUILD_GRAPH: REMOVED 2026-07-12. These were
/the old top-level driver for the pre-YAKU RO_* graph builder (read a
/quoted "template" string straight from the parser's own line buffer,
/build an RO_Lux graph from it). Zero callers remained project-wide —
/SYNTH_YAKU (synth.re) replaced this entirely (see DECISIONS.md), and
/the driver's own two named entry points (SEAL PS_DEF_BUILD_GRAPH
/PGB_SKIPWS; NOVA PS_DEF_BUILD_BODY Jump Exit=PGB_LOOP) both pointed
/*past* their own FOR-generated PGB_PS_FIRST/WORD/SHIFT reset block,
/making that reset permanently unreachable -- confirmed by inspecting
/the compiled instructions directly: {N}/{X} substitution was correct,
/the reset code itself was simply dead, unreached code. Since nothing
/called these entry points at all, the dead reset was harmless in
/practice, but the whole driver (PGB_SKIPWS/PEEK/STR_START/LOOP/DONE)
/is removed outright rather than fixed, matching this project's own
/precedent for RCALL/RRET and FUNC/ENDFUNC above.
/The low-level packed-string primitives below (PGB_ALLOC_LUX,
/PGB_FLUSH_WORD, PGB_EMIT_BYTE, PGB_FINALIZE, PGB_PLACEHOLDER, and the
/escape-byte emitters) are NOT part of this removal -- SYNTH_YAKU
/(synth.re, YK_* labels) calls them directly and depends on them.
//
============================================================
Escape byte map (used by PGB_PLACEHOLDER's dispatch below):
  {v1}→1 {v2}→2 {result}→3 {xlen}→4 {fresh}→5 {fresh2}→6
  {ptr_tgt}→7 {ldest}→8 {lnext}→9 {lnext_id}→11 {totalsz}→12
  {flb}→14 {vx0}→15 {vx1}→16 {vx2}→17 {vx3}→18 {vx8}→19
  ~ → LF (0x0A)
  Closing " → end of string
//

/── State for packed-string builder ──────────────────────────
NEW PGB_PS_FIRST     /addr of first packed lux (result)
NEW PGB_PS_PREV      /addr of previous packed lux (for chaining)
NEW PGB_PS_WORD      /accumulating u64 word (8 bytes)
NEW PGB_PS_SHIFT     /bit shift position (0, 8, 16, ... 56)
NEW PGB_PS_LUX      /addr of current lux being filled
NEW PGB_PS_PHBUF     /placeholder name bytes packed
NEW PGB_PS_PHI       /placeholder byte counter

/── PGB_ALLOC_LUX: bump-alloc 1 lux for packed string ──────
/Returns addr in PGB_PS_LUX. Chains from PGB_PS_PREV if set.
NOLINK
ALLOC_TO PGB_ALLOC_LUX Exit=PGB_PS_LUX Count=C_1
/If first lux: record as PGB_PS_FIRST
JZ PGB_AC_FCK El1=PGB_PS_FIRST Exit=PGB_AC_DONE
NOVA PGB_AC_SETF Op=Move El1=RA_ALLOC_RESULT Exit=PGB_PS_FIRST
/Explicit jump to PGB_AC_DONE -- NOLINK only blocks Python's own
/auto-chain bookkeeping, it does not create a real jump; this relied
/on PGB_AC_DONE's RREDI being physically adjacent, which landed on the
/unrelated PGB_FLUSH_WORD instead once allocation order shifted. Same
/class as this session's other Pattern Z fixes (see BUGS.md).
NOVA PGB_ACSJ Op=Jump Exit=PGB_AC_DONE
NOLINK
RREDI PGB_AC_DONE
/── PGB_FLUSH_WORD: write PGB_PS_WORD into PGB_PS_LUX, reset ─
NOLINK
RVOCA PGB_FLUSH_WORD Body=PGB_ALLOC_LUX
NOVA PGB_FW_WRITE Op=Write El1=PGB_PS_LUX El2=PGB_PS_WORD
CLEAR PGB_FW_RST_W El1=PGB_PS_WORD
CLEAR PGB_FW_RST_S El1=PGB_PS_SHIFT
RREDI PGB_FW_RET

/── PGB_EMIT_BYTE: pack one byte into current word ───────────
/Flushes word when shift==64. Does NOT alloc lux until flush.
/IN: RA_TMP = byte value
NOLINK
PACK_BYTE PGB_EMIT_BYTE PGB_PS_WORD PGB_PS_SHIFT RA_TMP Full=PGB_EB_FLUSH
RREDI PGB_EB_RET
NOLINK
/PGB_EB_FLUSH is the Full= jump target of the PACK_BYTE above: on a full
/word its JumpIf lands here (a direct jump to this node's address, not a
/deref), so this node itself must carry the label. It was named PGB_EB_FW
/and referenced nowhere, leaving PGB_EB_FLUSH undefined (word 0) -- the
/JumpIf then jumped into an empty cell and faulted the first time the load
/wave packed a full word. Same latent-undefined class as GL_IMPL.
RVOCA PGB_EB_FLUSH Body=PGB_FLUSH_WORD
RREDI PGB_EB_FRET

/── PGB_FINALIZE: flush partial word + write NUL terminator ──
NOLINK
/Flush partial word if shift > 0
JZ PGB_FINALIZE El1=PGB_PS_SHIFT Exit=PGB_FN_NUL
RVOCA PGB_FN_FW Body=PGB_FLUSH_WORD
/NUL terminator lux (word=0)
CLEAR PGB_FN_NUL El1=PGB_PS_WORD
RVOCA PGB_FN_NALLOC Body=PGB_ALLOC_LUX
NOVA PGB_FN_NWRITE Op=Write El1=PGB_PS_LUX El2=C_0
RREDI PGB_FN_RET

/── PGB_PLACEHOLDER: read {name}, emit escape byte ───────────
/Reads bytes straight from the stream (BS_READ_BYTE) until '}' or EOF;
/matches against known names, emits escape. Unknown → emits '?' (0x3F).
NEW PGB_PH_NAME0   /first 8 bytes of name packed
NEW PGB_PH_NLEN    /name length
NEW PGB_PH_SHIFT2  /packing shift
NOLINK
CLEAR PGB_PLACEHOLDER El1=PGB_PH_NAME0
CLEAR PGB_PH_INIT1 El1=PGB_PH_NLEN
CLEAR PGB_PH_INIT2 El1=PGB_PH_SHIFT2
/Read bytes until '}' (or EOF, same treatment as running out used to get)
RVOCA PGB_PH_LOOP Body=BS_READ_BYTE
JZ PGB_PH_EOFCK El1=RA_LOAD_BYTE Exit=PGB_PH_DISPATCH
JEQ PGB_PH_RBJ El1=RA_LOAD_BYTE El2=RBRACE Exit=PGB_PH_DISPATCH
/Pack into name0 (first 8 bytes)
NOVA PGB_PH_LENCK Op=ULess El1=PGB_PH_NLEN El2=C_8 Exit=RA_TMP2
NOVA PGB_PH_LENJ Op=JumpIf El1=RA_TMP2 Exit=PGB_PH_PACK
NOVA PGB_PH_SKIP Op=Jump Exit=PGB_PH_LOOP
NOLINK
NOVA PGB_PH_PACK Op=Left El1=RA_LOAD_BYTE El2=PGB_PH_SHIFT2 Exit=RA_TMP2
NOVA PGB_PH_OR Op=Or El1=PGB_PH_NAME0 El2=RA_TMP2 Exit=PGB_PH_NAME0
NOVA PGB_PH_SINC Op=Add El1=PGB_PH_SHIFT2 El2=C_8 Exit=PGB_PH_SHIFT2
NOVA PGB_PH_INC Op=Add El1=PGB_PH_NLEN El2=C_1 Exit=PGB_PH_NLEN
NOVA PGB_PH_JMP Op=Jump Exit=PGB_PH_LOOP
/Dispatch: compare PGB_PH_NAME0 against known names (packed little-endian)
/Names: "v1"=0x3176, "v2"=0x3276, "result"=..., etc.
NOLINK
RREDI PGB_PH_DISPATCH
/Compare name0 against packed strings
/v1 = 'v'=0x76, '1'=0x31 → packed LE = 0x3176
SET PGB_N_V1 Value=12662 /0x3176 = "v1" packed
SET PGB_N_V2 Value=12918 /0x3276 = "v2" packed
/result = 0x746C7573657266  (r=0x72,e=0x65,s=0x73,u=0x75,l=0x6C,t=0x74)
SET PGB_N_RESULT Value=128009175786866 /0x746C75736572 = "result" packed
/fresh = 0x687365726600 (f=0x66,r=0x72,e=0x65,s=0x73,h=0x68)
SET PGB_N_FRESH Value=448612627046 /0x6873657266 = "fresh" packed
/xlen = 0x6E656C78 (x=0x78,l=0x6C,e=0x65,n=0x6E)
SET PGB_N_XLEN Value=1852140664 /0x6E656C78 = "xlen" packed
/ldest = 0x74736564_6C (l=0x6C,d=0x64,e=0x65,s=0x73,t=0x74)
SET PGB_N_LDEST Value=500152231020 /0x747365646C = "ldest" packed
/lnext = 0x7478656E6C (l=0x6C,n=0x6E,e=0x65,x=0x78,t=0x74)
SET PGB_N_LNEXT Value=500236119660 /0x7478656E6C = "lnext" packed
SWITCH PGB_PH_NAME0
    PGB_N_V1      PGB_PH_E1
    PGB_N_V2      PGB_PH_E2
    PGB_N_RESULT  PGB_PH_E3
    PGB_N_FRESH   PGB_PH_E5
    PGB_N_XLEN    PGB_PH_E4
    PGB_N_LDEST   PGB_PH_E8
    PGB_N_LNEXT   PGB_PH_E9
    PGB_N_FRESH2  PGB_PH_E6
    PGB_N_PTR_TGT PGB_PH_E7
    PGB_N_LNEXT_ID PGB_PH_E11
    PGB_N_TOTALSZ PGB_PH_E12
    PGB_N_FLB     PGB_PH_E14
    PGB_N_VX0     PGB_PH_E15
    PGB_N_VX1     PGB_PH_E16
    PGB_N_VX2     PGB_PH_E17
    PGB_N_VX3     PGB_PH_E18
    PGB_N_VX8     PGB_PH_E19
    PGB_N_CMP     PGB_PH_E10
    PGB_N_ARITH   PGB_PH_E13
/Unknown placeholder → '?' (0x3F)

NOVA PGB_PH_UNK Op=Move El1=QMARK Exit=RA_TMP
RVOCA PGB_PH_UEB Body=PGB_EMIT_BYTE
RREDI PGB_PH_URET
/── Escape byte emitters ──────────────────────────────────────────────────
FOR PGB_PH_E1 PGB_PH_E2 PGB_PH_E3 PGB_PH_E4 PGB_PH_E5 PGB_PH_E6 PGB_PH_E7
    NOLINK
    NOVA {X} Op=Move El1=C_{N} Exit=RA_TMP
    RVOCA {X}B Body=PGB_EMIT_BYTE
    RREDI {X}R

FOR PGB_PH_E8 PGB_PH_E9 PGB_PH_E10 PGB_PH_E11 PGB_PH_E12 PGB_PH_E13 PGB_PH_E14
    NOLINK
    NOVA {X} Op=Move El1=C_{N} Exit=RA_TMP
    RVOCA {X}B Body=PGB_EMIT_BYTE
    RREDI {X}R

FOR PGB_PH_E15 PGB_PH_E16 PGB_PH_E17 PGB_PH_E18 PGB_PH_E19
    NOLINK
    NOVA {X} Op=Move El1=C_{N} Exit=RA_TMP
    RVOCA {X}B Body=PGB_EMIT_BYTE
    RREDI {X}R

/── Main loop: REMOVED 2026-07-12 (see note above PGB_PS_FIRST) ──────────
/PS_DEF_BUILD_GRAPH, PS_DEF_BUILD_BODY, and their shared driver
/(PGB_SKIPWS/PGB_PEEK*/PGB_STR_START/PGB_LOOP/PGB_DONE — read a quoted
/or unquoted template string from the parser's own line buffer, byte by
/byte, calling PGB_EMIT_BYTE/PGB_PLACEHOLDER/PGB_FINALIZE) had zero
/callers project-wide. SYNTH_YAKU (synth.re) is the live replacement.

/── Command handler registrations — lux.word = handler entrypoint
/Required for BS_LOOKUP dispatch: hash(name) → lux → Read lux.word → JumpReg
/Note: NOLINK and YAKU/YAKU_NEXO* are NOT listed here — expand.re/synth.re
/define them first and LOAD_DISPATCH_CORE handles NOLINK via first-byte SWITCH
/('N' path), while YAKU reaches synth.re's real macro via BS_LOOKUP →
/SYNTH_YAKU (same as YAKU_NEXO always did before the unification) --
/an earlier "SEAL YAKU PS_LINE_WAX" line here collided with that exact
/registration (see BUGS.md Pattern Y); removed.



//
────────────────────────────────────────────────────────────────
Below: cold-boot line/grammar parsing, moved here from wax.re.
Mechanical move, not a rewrite -- see HISTORY.md for the date/reason.

This is LOAD_MAIN's (wax.re) own grammar layer -- recognizing NOVA/
NEW/SET/LINK/SEAL/SPAN/FOR/etc, reading El1=/El2=/Exit=/Name= key=value
tokens, and the indent-aware CHAIN/SAVE/FOR/NOVAE block reader -- called
by wax.re's wave loop, per record. It sits alongside PS_MAIN's own
independent grammar above rather than merged into it: PS_MAIN's own
"hot reload, already-running program" rationale for staying separate
from LOAD_MAIN's parsing turned out not to hold once a real prepass
(tide) existed -- there was never a genuine reason to keep two parsers.
That dedup (unifying this block with PS_MAIN) is intentionally NOT
done yet -- this move only relocates the code, unchanged.
────────────────────────────────────────────────────────────────
//

── Parser internal state (line/block grammar) ─────────────────
/Block-comment state
/Body buffer (FOR/SAVE template expansion)
SET SK_BODY_BUF_SIZE Value=4096
NEW SK_BODY_BUF  Count=4096
NEW SK_BODY_BASE   /base addr of SK_BODY_BUF (set at init)
SET LD_BODY_BUF_BASE_VAL Value=SK_BODY_BASE /alias: expand.re uses LD_BODY_BUF_BASE_VAL
NEW SK_BODY_PTR    /write position
SET LD_BODY_PTR Value=SK_BODY_PTR /alias: expand.re uses LD_BODY_PTR
NEW SK_BODY_LIMIT  /base + size: overflow guard
NEW SK_BODY_LINES  /number of lines in body buf
/Indent-level stack (new LOAD_READ_LINE -- see BUGS.md/CONVENTIONS.md
/for the rule this implements): one word per known level, level 0 =
/the opener's own indent. 16 levels is generous headroom -- confirmed
/project-wide zero real cases of nested macro-in-macro today, so even
/depth 3-4 would already be unprecedented.
SET SK_IND_STACK_SIZE Value=16
NEW SK_IND_STACK  Count=16
NEW SK_IND_STACK_BASE  /base addr of SK_IND_STACK (set at init, SPAN doesn't self-ref)
NEW SK_IND_TOP     /index of the current deepest known level (0 = opener's own line)
NEW SK_IND_COUNT   /exact indent (spaces, tab=4) of the line just read by LOAD_READ_LINE
NEW SK_IND_ACTION  /classify() result: 0=skip 1=same 2=deeper 3=return
NEW SK_IND_LEVEL   /classify() result: matched/pushed level index (only meaningful if ACTION != 0/skip)
NEW SK_IC_SCAN_I   /LOAD_INDENT_CLASSIFY's own stack-scan loop index
NEW SK_IC_CNT_P    /LOAD_COUNT_INDENT's own walk pointer

/── Context stack (PUSH-model block handling) ──────────────────
//
Mirrors Python's _ctx_stack in _parse_indent_blocks. Each slot holds
the state of one open block command (SWITCH/FOR/SAVE/YAKU/MATCH/SOMA).
The dispatch loop pushes a slot when a block opener is encountered,
feeds body lines to the current context, and pops+closes on dedent.

Slot layout (CTX_SLOT_SIZE = CTX_HEADER_SIZE + SK_IND_STACK_SIZE + 1):
  [0]                        mode (CTX_MODE_*)
  [1]                        opener_indent
  [2]                        ind_top (saved SK_IND_TOP)
  [CTX_HEADER_SIZE .. CTX_HEADER_SIZE+SK_IND_STACK_SIZE-1]
                             ind_levels (saved indent stack)
  [CTX_OFF_DATA_PTR]         pointer to mode-specific data (bump-allocated)

All derived sizes computed at runtime in LOAD_MAIN from
CTX_HEADER_SIZE(3), SK_IND_STACK_SIZE, and CTX_MAX_DEPTH.
//
SET CTX_HEADER_SIZE Value=3
SET CTX_MAX_DEPTH Value=8
NEW CTX_SLOT_SIZE        /computed: CTX_HEADER_SIZE + SK_IND_STACK_SIZE + 1
NEW CTX_OFF_DATA_PTR     /computed: CTX_HEADER_SIZE + SK_IND_STACK_SIZE
NEW SK_CTX_SLOTS_COUNT   /computed: CTX_SLOT_SIZE * CTX_MAX_DEPTH
NEW SK_CTX_SLOTS_BASE    /set by ALLOC_TO in LOAD_MAIN
NEW SK_CTX_SP           /stack pointer: count of open contexts (0 = empty, top slot at SP-1)
/mode constants
SET CTX_MODE_NONE    Value=0
SET CTX_MODE_SWITCH  Value=1
SET CTX_MODE_FOR     Value=2
SET CTX_MODE_SAVE    Value=3
SET CTX_MODE_YAKU    Value=4
SET CTX_MODE_MATCH   Value=5
SET CTX_MODE_SOMA    Value=6
/slot field offsets (CTX_OFF_DATA_PTR is runtime-computed, rest are header)
SET CTX_OFF_MODE     Value=0
SET CTX_OFF_INDENT   Value=1
SET CTX_OFF_IND_TOP  Value=2
SET CTX_OFF_IND_LVL  Value=3  /== CTX_HEADER_SIZE (first slot after fixed header)
/scratch for push/pop/peek
NEW SK_CTX_TMP       /scratch addr during push/pop
NEW SK_CTX_SLOT_ADDR /addr of the current top slot (SK_CTX_SLOTS_BASE + SK_CTX_SP * CTX_SLOT_SIZE)

/SK_SEAL_NAMEPOOL / SK_POOL_* removed: the private name pool (and its
/LOAD_POOL_NAME copy loop) existed only so backfill could re-look-up a
/forward-referenced name later. Forward refs now record the name's HT
/entry address directly (BS_ENTRY_ADDR, intern.re), so no name copy is
/kept and the whole pool -- and its never-reset unbounded growth -- is gone.
NEW SK_ABF_WVAL   /backfill resolve: an entry's raw word0 ((hash<<32)|lid)
NEW SK_ABF_LID    /backfill resolve: the located lid extracted from word0
/Scratch
NEW SK_TMP
SET LD_TMP Value=SK_TMP /alias: expand.re uses LD_TMP as shared loader scratch
NEW SK_TMP2
NEW SK_TMP3
NEW SK_KDEST  /keyed store: destination addr (KEY_DEST[code]) held across the
              /LOAD_ELEM_RESOLVE call, which clobbers SK_TMP -- see LOAD_RKVA
NEW SK_FLAG
SET SK_FLAG /self-ref: Equal writes here, JumpIf reads here
SET LD_FLAG Value=SK_FLAG /alias: expand.re uses LD_FLAG
NEW SK_WB_NAME_PTR In=OWN  /bump pointer into SK_BOOT_NAMEBUF, one word's
                            /own start address captured per word below,
                            /matching HT_NAME_POOL_PTR's own bump-alloc
                            /convention (htable.re) -- avoids needing a
                            /separate numeric offset constant per word.
/Macro arg reading
NEW SK_ELEM_VALUE      /resolved addr from LOAD_MA_READARG (0=done)
NEW SK_ELEM_HASH        /hash of most recently read arg token
NEW SK_ELEM_KIND       /kind of the token LOAD_ELEM_RESOLVE just resolved:
                       /1 = integer literal (SK_ELEM_VALUE is a constant
                       /cell; a consumer wanting the NUMBER derefs it once),
                       /0 = anything else -- a name or a "..." string, whose
                       /SK_ELEM_VALUE is already the address to store as-is.
                       /The one bit that survives eager address-resolution:
                       /Python stays lazy (args are strings, kind always
                       /recoverable), the reko reader resolves to an address
                       /and would otherwise forget whether the source was a
                       /number. Load-time only -- never consulted at runtime.
                       /The generic keyed store writes it into SK_ARG_<key>[1]
                       /(the slot adjacent to the value), so SET/Count/Width read
                       /the number vs the address correctly with no per-command
                       /bypass.
NEW SK_MA_HASH0       /hash of MA0 token (for _J/_K name extension)
/── LOAD_PACK_STRING scratch (pack a "..." tokbuf literal into a lux chain) ──
NEW SK_PS_I            /LOAD_PACK_STRING: byte index within BS_TOKBUF (from 1)
NEW SK_PS_LIM          /LOAD_PACK_STRING: stop index (tlen-1, the closing ")
NEW SK_PS_ADDR         /LOAD_PACK_STRING: BS_TOKBUF_BASE + i, cell to Read
NEW RA_MC_JTARGET  /LOAD_MA_AUTOCOMPLETE_J: address of the caller's own MA3/MA4
NEW SK_NOVA_ADDR       /current NOVA lux addr
/SK_NOVA_NEW is gone. It existed to answer one question -- "was this lux
/freshly allocated?" -- and the only thing that ever asked was the branch
/that skipped linking an already-known lux into the chain. That branch
/was itself the bug (the Python loader links unconditionally), so once it
/went the flag had nothing left to tell anyone.
NEW SK_NOVA_E1         /parsed El1 addr
NEW SK_NOVA_E2         /parsed El2 addr
NEW SK_NOVA_EXIT       /parsed Exit addr
NEW SK_NOVA_LCNT       /lumen count from Wave 1 (lcount) -- moved here from
                       /wax.re alongside EXP_CMD_NOVA, its only user
/EXP_CMD_NOVA: name is the keyless token, found by exclusion, not
/position -- the bare op token may come before or after it, either
/order, told apart purely by which one has no key (see EXP_CMD_NOVA's
/own docstring). No Name= keyword form anymore (retired everywhere,
/including WAX_CMD).
NEW SK_CI_ANON        /1 if this NOVA has no keyless token anywhere (genuinely anonymous)
NEW SK_CI_OPBUF_LEN   /stashed op token's length, when read first
NEW SK_CI_OPBUF_HASH  /stashed op token's RA_LOAD_HASH -- BS_INTERN
                           /trusts this directly, doesn't recompute it from
                           /raw bytes, so it must be stashed alongside them
NEW SK_CI_ST_I        /stash/unstash byte-copy loop index
NEW SK_CRV_SUBFIRST   /1 if the keyed (op/sub) token was read before the name
NEW SK_CI_OPBUF_000  Count=64  /stash for the op token's raw bytes,
                                /read before the name turned up
NEW SK_CI_OPBUF_BASE      /copy of SK_CI_OPBUF_000's own value, used
                                /for all addressing -- SK_CI_OPBUF_000
                                /itself is the buffer's first DATA lux; the
                                /stash loop writes into it starting at
                                /offset 0, so addressing off SK_CI_OPBUF_000
                                /directly would have the very first byte
                                /written overwrite the base pointer's own
                                /self-ref, corrupting every later offset in
                                /the same call. Confirmed directly: RA_FLUX_
                                /BOTTOM (address 98) got overwritten with a
                                /stray ASCII byte this way. Same split as
                                /BS_TOKBUF_000/BS_TOKBUF_BASE in lexer.re.
/Backfill intern
NEW SK_ELEM_EXIT    /slot addr to fill (set before LOAD_INTERN call)
/LOAD_ELEM: classify current tokbuf's key
NEW SK_ELEM_KEY       /canonical key code: 0=positional/no '=', otherwise the
                           /key's numeric code (1..N) assigned by KEY_HTAB in
                           /sorted-key order; the caller routes by code through
                           /KEY_DEST[code]. See LOAD_ELEM's docstring, lexer.re
/One 2-wide SK_ARG_<key> slot (value at [0], kind at [1]) per key in the
/discovered vocabulary -- the uniform keyed-argument namespace the generic
/store writes into (mirrors Python's slots[key]). Name is the only key that
/does not get one: it folds into RA_MA0, the identity slot the "first keyless
/token == Name" rule also fills.
AUTOGEN_KEY_REGS

/── Data-driven key table ──────────────────────────────────────────────────
/A real hash table of the key vocabulary, pre-computed and written as static
/data (see loader.py AUTOGEN_KEY_TABLE). Recognition is one HT_LOOKUP into it
/(LOAD_ELEM, lexer.re) in place of the old MATCH byte-tree; the keyed store
/reads KEY_DEST[code] in place of the old per-key JEQ/store chain.
AUTOGEN_KEY_TABLE KEY

/Cmd dispatch
NEW SK_LINES_DONE     /lines actually dispatched, whole run
NEW SK_CMDS_DONE      /lines that reached a real command handler
NEW SK_SKIPPED_DONE   /lines whose command lux carried no handler
NEW SK_NONAME_DONE    /lines whose command name was not in the table at all
NEW SK_CMD_ADDR       /current command lux addr (set by LOAD_DISPATCH_LINE)
NEW RA_CU_CONTINUE  /LOAD_CU_RESOLVE/LOAD_CU_PREPARE: 1=continue to the next stage, 0=this line is already fully handled (BS_SKIP_TO_EOL already ran via LOAD_CU_SKPL) -- caller has nothing left to do
/Template expansion
NEW SK_ET_PTR         /read position in body buf during expansion
NEW SK_ET_OUT         /write position in output (reuses BS_TOKBUF_BASE)
NEW SK_ET_DONE        /1 once the final (end-of-body) flush has run — without
                       /this, LOAD_EXPAND_TEMPLATE's loop never terminates:
                       /once SK_ET_PTR reaches SK_BODY_PTR, neither value ever
                       /changes again, so the same "end of body" branch would
                       /re-fire (and re-dispatch an empty line) forever
NEW SK_ET_PH_IDX      /placeholder name scan index
/FOR elem name
NEW SK_FOR_ELEM       /packed addr of current FOR element name string
/SAVE_EMIT scratch
NEW SK_SES_SREG       /current S_reg addr
NEW SK_SES_SDST       /target SAVE_ES_SREG_N slot
NEW SK_SES_SRC        /SAVE_EMIT_MOVE_LUX: El1 of the Move it builds
NEW SK_SES_DST        /SAVE_EMIT_MOVE_LUX: Exit of the Move it builds
NEW SK_SES_SREG_1 In=SAVE
SET SK_SES_SREG_1
NEW SK_SES_SREG_2 In=SAVE
SET SK_SES_SREG_2
NEW SK_SES_SREG_3 In=SAVE
SET SK_SES_SREG_3
NEW SK_SES_SREG_4 In=SAVE
SET SK_SES_SREG_4
/SAVE data area size: 4 words for RA_MA1..4 + 4 words for SK_SES_SREG_1..4 values
SET SAVE_DATA_SIZE Value=8
/LOAD_INTERN_SETUP scratch
SET SK_ALLOC_NOVA_OP Value=__LT_ALLOC_NOVA /word = addr(__LT_ALLOC_NOVA); sentinel for detection
/LOAD_MA_READARG: integer → C_N address mapping
SET SK_MA0_ADDR Value=RA_MA0 /addr of RA_MA0 for builder detection
/Emit-int scratch
NEW SK_EIB_VAL
NEW SK_EIB_DIV
NEW SK_POS_SLOT       /positional slot index (1..7) for LOAD_READARG_KV

── LOAD_DISPATCH_LINE: read first token, dedent-close, dispatch ─
//
PUSH-model dispatch (mirrors Python _parse_indent_blocks):
  1. Measure indent (LOAD_COUNT_INDENT -> SK_IND_COUNT)
  2. Read first token (BS_READ_TOKEN)
  3. Blank/comment -> return (tlen==0)
  4. Dedent-close loop: while SK_CTX_SP > 0 and
     SK_IND_COUNT <= top.opener_indent -> close + pop
  5. If still in a context -> route to mode handler
  6. Otherwise -> normal LOAD_DISPATCH_CORE
//
NOLINK
RVOCA LOAD_DISPATCH_LINE Body=LOAD_COUNT_INDENT
RVOCA LOAD_DL_TOK Body=BS_READ_TOKEN
JZ LOAD_DL_BLNK El1=RA_LOAD_TLEN Exit=LOAD_DL_COUNT
/── dedent-close loop: pop contexts whose opener_indent >= SK_IND_COUNT ──
/SK_CTX_SP == 0 -> no context open -> normal dispatch
JZ LOAD_DL_DDK El1=SK_CTX_SP Exit=LOAD_DL_NORMAL
/peek top slot, read opener_indent
RVOCA LOAD_DL_PEEK Body=LOAD_CTX_PEEK
NOVA LOAD_DL_OI_A Op=Add El1=SK_CTX_SLOT_ADDR El2=CTX_OFF_INDENT Exit=SK_TMP
NOVA LOAD_DL_OI_R Op=Read El1=SK_TMP Exit=SK_TMP2
/test: SK_IND_COUNT > opener_indent?
/compute opener_indent - SK_IND_COUNT; if underflow (high bit set) -> cur > opener
NOVA LOAD_DL_DSUB Op=Sub El1=SK_TMP2 El2=SK_IND_COUNT Exit=SK_TMP
/equal -> close
JZ LOAD_DL_DEQ El1=SK_TMP Exit=LOAD_DL_DCLOSE
/check high bit: right-shift by 63
NOVA LOAD_DL_DSHI Op=Right El1=SK_TMP El2=C_63 Exit=SK_TMP3
/high bit 1 -> underflow -> cur > opener -> still inside -> route
JZ LOAD_DL_DHCK El1=SK_TMP3 Exit=LOAD_DL_DCLOSE
/cur > opener: still inside this context
NOVA LOAD_DL_DINSIDE Op=Jump Exit=LOAD_DL_CTX_ROUTE
NOLINK
/close the top context (mode-specific finalization), then pop and recheck
NOVA LOAD_DL_DCLOSE Op=Jump Exit=LOAD_DL_DCLOSE_IMPL
NOLINK
RVOCA LOAD_DL_DCLOSE_IMPL Body=LOAD_CTX_CLOSE_TOP
RVOCA LOAD_DL_DPOP Body=LOAD_CTX_POP
/loop: check next context
NOVA LOAD_DL_DLOOP Op=Jump Exit=LOAD_DL_DDK
NOLINK
/── context routing: we are inside a context ──
RVOCA LOAD_DL_CTX_ROUTE Body=LOAD_INDENT_CLASSIFY
/SK_IND_ACTION: 0=skip 1=same 2=deeper 3=return
JZ LOAD_DL_SKIP El1=SK_IND_ACTION Exit=LOAD_DL_COUNT
/read mode from top slot[0]
NOVA LOAD_DL_RMODE Op=Read El1=SK_CTX_SLOT_ADDR Exit=SK_TMP2
/mode jump table. PULL commands (FOR/SWITCH/YAKU) consume their own body
/before the main loop sees it, so their mode is never active here.
/SAVE body is passthrough. SOMA/MATCH are stubs (Python-side only).
/All modes fall through to normal dispatch.
JEQ LOAD_DL_M_SOMA El1=SK_TMP2 El2=CTX_MODE_SOMA Exit=LOAD_DL_NORMAL
JEQ LOAD_DL_M_FOR El1=SK_TMP2 El2=CTX_MODE_FOR Exit=LOAD_DL_NORMAL
JEQ LOAD_DL_M_SW El1=SK_TMP2 El2=CTX_MODE_SWITCH Exit=LOAD_DL_NORMAL
JEQ LOAD_DL_M_MATCH El1=SK_TMP2 El2=CTX_MODE_MATCH Exit=LOAD_DL_NORMAL
/── normal dispatch (no context, or save/yaku passthrough) ──
RVOCA LOAD_DL_NORMAL Body=LOAD_DISPATCH_CORE
NOVA LOAD_DL_COUNT Op=Add El1=SK_LINES_DONE El2=C_1 Exit=SK_LINES_DONE
RREDI LOAD_DL_RET

── LOAD_DISPATCH_CORE: dispatch whatever is already in BS_TOKBUF_BASE ─
//
Shared by LOAD_DISPATCH_LINE (fresh-read-then-dispatch, the common
case) and LOAD_DISPATCH_BUILT_LINE (dispatch already-constructed
content, used by CHAIN body processing -- see BUGS.md: these
previously called LOAD_DISPATCH_LINE directly, which silently
discarded their carefully-built "NOVA name op args" text by
immediately overwriting BS_TOKBUF_BASE with a fresh file-stream read).
IN: BS_TOKBUF_BASE/RA_LOAD_TLEN already set by the caller.
//
SET LOAD_DISPATCH_CORE Value=LOAD_DC_TLENCK
NOLINK
JZ LOAD_DC_TLENCK El1=RA_LOAD_TLEN Exit=LOAD_DC_RET
/Look the whole token up and go where it says. The hash was computed
/once, by tide, before any of this ran; BS_LOOKUP turns it into the
/handler's address exactly.
/A SWITCH on the first byte used to sit here, after this same lookup,
/re-deriving by hand what the hash had already answered -- and doing it
/worse: NOLINK and a command like NOVA share their first two bytes, so
/each new command risked needing another byte of disambiguation. Every
/command it reached is named in the table now, so it is gone. This is
/the same path parser.re takes: look up, jump.
RVOCA LOAD_DC_LK Body=BS_LOOKUP
NOVA LOAD_DC_SAVE Op=Move El1=RA_BS_RESULT Exit=SK_CMD_ADDR
RVOCA LOAD_DC_UCMD Body=LOAD_CMD_UNKNOWN
RREDI LOAD_DC_RET

── LOAD_DISPATCH_BUILT_LINE: dispatch already-constructed content ────
//
For CHAIN body processing: the build-name helpers write
"NOVA name op args" into BS_TOKBUF_BASE and set RA_LOAD_TLEN to its
length; this dispatches THAT content directly. Activates redirect mode
(see registers.re/BS_READ_BYTE) so that nested BS_READ_TOKEN calls made
BY THE COMMAND HANDLER ITSELF (e.g. EXP_CMD_NOVA reading the name, then
the op, then El1=/El2=/Exit= via LOAD_NOVA_READ_ARGS) also read from the
constructed text -- not just the first token. Without this, only the
outermost dispatch avoided the live-stream-reread bug; every handler's
OWN internal token reads still hit the live file stream, picking up
unrelated text from wherever the file position happened to be (since
the whole CHAIN body was already consumed into a separate buffer
by LOAD_READ_BODY before body-walking even began) -- see BUGS.md.
//
NOLINK
NOVA LOAD_DISPATCH_BUILT_LINE Op=Move El1=BS_TOKBUF_BASE Exit=RA_REDIRECT_BASE
NOVA LOAD_DBL_RLEN2 Op=Move El1=RA_LOAD_TLEN Exit=RA_REDIRECT_LEN
CLEAR LOAD_DBL_RPOS2 El1=RA_REDIRECT_POS
RVOCA LOAD_DBL_CORE Body=LOAD_DISPATCH_CORE
CLEAR LOAD_DBL_ROFF El1=RA_REDIRECT_BASE
RREDI LOAD_DBL_RET


── LOAD_READ_NAME: read a naming command's own label ─────────
//
Reads the next token and strips a `Name=` prefix if present, leaving the
bare label in BS_TOKBUF with RA_LOAD_HASH recomputed for it.

Both forms of a naming command's label go through here:
    NOVA FOO Op=Move ... -- positional (original)
    NOVA FOO Op=Move ... -- keyword (new)
BS_TOKEN_VALUE returns a token containing no '=' untouched, so the
positional form costs one extra call and changes nothing about it.

This exists so the rule lives in ONE place rather than in each naming
handler: every command that declares a label calls this instead of
BS_READ_TOKEN for its first token. The Wave 1 prepass declaration
scanners (LOAD_PP_*) call it too -- if they read `Name=FOO` literally,
lcount would key sizes on the wrong string and under-allocate.
//
NOLINK
RVOCA LOAD_READ_NAME Body=BS_READ_TOKEN
RVOCA LOAD_RN_STRIP Body=LOAD_ELEM
RREDI LOAD_RN_RET

── LOAD_PP_TMPL_CHECK: scan token for '{' (template placeholder) ──
//
IN:  BS_TOKBUF_BASE[0..RA_LOAD_TLEN-1] holds the current token.
OUT: SK_TMP = 1 if '{' found, 0 if not.
Wave 1 prepass parsing predicate (called by wax.re's LOAD_PP_* scanners
alongside LOAD_READ_NAME above) -- lives here with the other token-content
classifiers, not in the loader/wave orchestration.
//
NOLINK
CLEAR LOAD_PP_TMPL_CHECK El1=SK_TMP2
JEQ LOAD_PP_TCL El1=SK_TMP2 El2=RA_LOAD_TLEN Exit=LOAD_PP_TC_NF
NOVA LOAD_PP_TCA Op=Add El1=BS_TOKBUF_BASE El2=SK_TMP2 Exit=SK_TMP
NOVA LOAD_PP_TCR Op=Read El1=SK_TMP Exit=SK_TMP
JEQ LOAD_PP_TCCMP El1=SK_TMP El2=LBRACE Exit=LOAD_PP_TC_FND
NOVA LOAD_PP_TCINC Op=Add El1=SK_TMP2 El2=C_1 Exit=SK_TMP2
NOVA LOAD_PP_TCJMP Op=Jump Exit=LOAD_PP_TCL
NOLINK
NOVA LOAD_PP_TC_FND Op=Move El1=C_1 Exit=SK_TMP
RREDI LOAD_PP_TC_FRET
NOLINK
CLEAR LOAD_PP_TC_NF El1=SK_TMP
RREDI LOAD_PP_TC_NRET


── LOAD_CI_STASH_TOK / LOAD_CI_UNSTASH_TOK removed ──────────
/op-token staging existed only so EXP_CMD_NOVA could read the op
/token before knowing whether a name followed, then restore it. The
/pre-read reads name and op independently now (MA0 / SK_ARG_Op), so
/there is nothing to stash; both helpers had no other caller.

── LOAD_CMD_UNKNOWN: lookup SK_CMD_ADDR → resolve, gate, call handler ─
//
SK_CMD_ADDR = addr of cmd lux (set by LOAD_DISPATCH_LINE). Thin
orchestrator over three independently RVOCA-callable pieces below --
LOAD_CU_RESOLVE (find the handler, with accounting), LOAD_CU_PREPARE
(WAX_CMD-bypass / declare-only gate / generic MA0..MA7 pre-read), and
the final Voca. Split out so a caller that only needs one piece (e.g.
"just resolve this word to a handler address, I don't need the generic
argument pre-read") can call that piece directly instead of paying for
the whole sequence -- found while looking at why NEW/SET/LINK/NOVA (all
native, self-token-reading handlers, same family as WAX_CMD) still had
to run past LOAD_CU_PREPARE's WAX_CMD check and declare-only gate to
reach their own dispatch. Not resolved by this split alone (each piece
still runs its full body when called) -- but a future MATCH-based
dispatch can now route directly to whichever piece(s) a given command
actually needs, once that table exists.
//
NOLINK
RVOCA LOAD_CMD_UNKNOWN Body=LOAD_CU_RESOLVE
JZ LOAD_CU_RCK El1=RA_CU_CONTINUE Exit=LOAD_CU_RET
RVOCA LOAD_CU_PCALL Body=LOAD_CU_PREPARE
JZ LOAD_CU_PCK El1=RA_CU_CONTINUE Exit=LOAD_CU_RET
/No RA_MC_PREV reseed/writeback here anymore -- RA_MC_PREV already
/flows forward correctly across nested macro calls on its own (the
/way Python's single _last_nova does); the old reseed-from-BS_LAST_NOVA-
/before/writeback-after dance was both unnecessary and, on inspection,
/broken (see EXP_CMD_NOLINK's comment for the full explanation).
NOVA LOAD_CU_BCALL Op=Voca El1=SK_TMP2 Exit=RA_LINK
RREDI LOAD_CU_RET

── LOAD_CU_RESOLVE: find SK_CMD_ADDR's own handler, with accounting ──
//
IN:  SK_CMD_ADDR.
OUT: on success -- SK_TMP = handler address, RA_CU_CONTINUE = 1.
     on failure -- RA_CU_CONTINUE = 0; BS_SKIP_TO_EOL has ALREADY run
     (via the shared LOAD_CU_SKPL exit) by the time this returns, so a
     caller seeing RA_CU_CONTINUE = 0 has nothing left to do for this
     line -- not even a second skip.

Two genuinely distinct "unknown" cases, checked separately and in this
order:
1. SK_CMD_ADDR itself is 0 -- BS_LOOKUP found no symbol at all for this
   line's first token. This is the *design*, not an error condition:
   loader.py's own _parse_line docstring states it explicitly -- "a
   line that starts with an unrecognised token is simply ignored (it
   IS the comment)" -- e.g. this project's own "====...====" divider
   lines (auto_link.re, limiter.re, math.re, wane.re, yaku.re --
   files that start with exactly this line, none of them prefixed
   with a comment marker). Missing this check meant
   "Read El1=SK_CMD_ADDR" (below) double-dereferenced through zero --
   aether[0] itself, not aether[aether[SK_CMD_ADDR]] as intended --
   picking up whatever unrelated value happened to occupy address 0
   (in practice, some other allocation's own address, since address 0
   is otherwise just an ordinary lux), and *that* looked like a valid,
   nonzero jump target to the check further down. Confirmed directly:
   aether[0] equalled the real session's own observed crash PC exactly.
   See BUGS.md Pattern Z.
2. SK_CMD_ADDR is a real, found symbol, but that symbol's own word is
   0 (an intern'd-but-never-registered name, e.g. a builder macro that
   exists as a name but has no real target wired to it yet).
//
NOLINK
/Three outcomes, three counters. "Name not found at all" and "found but
/carrying no handler" are different failures with different causes, and
/collapsing them is how 13k silent drops looked like one problem.
JZ LOAD_CU_RESOLVE El1=SK_CMD_ADDR Exit=LOAD_CUR_NONAME
NOVA LOAD_CUR_NAMEOK Op=Jump Exit=LOAD_CUR_RD
NOLINK
NOVA LOAD_CUR_NONAME Op=Add El1=SK_NONAME_DONE El2=C_1 Exit=SK_NONAME_DONE
NOVA LOAD_CUR_NCONT Op=Move El1=C_0 Exit=RA_CU_CONTINUE
NOVA LOAD_CUR_NNJ Op=Jump Exit=LOAD_CU_SKPL
NOLINK
NOVA LOAD_CUR_RD Op=Read El1=SK_CMD_ADDR Exit=SK_TMP
/Split the two outcomes apart. A line that reaches a handler and a line
/dropped because its command has no handler are very different facts,
/and lumping them together is what let 13652 silent drops hide behind a
/healthy-looking line count.
JZ LOAD_CUR_CK El1=SK_TMP Exit=LOAD_CUR_NOHANDLER
NOVA LOAD_CUR_CCOUNT Op=Add El1=SK_CMDS_DONE El2=C_1 Exit=SK_CMDS_DONE
NOVA LOAD_CUR_CONT1 Op=Move El1=C_1 Exit=RA_CU_CONTINUE
RREDI LOAD_CUR_CRET
NOLINK
NOVA LOAD_CUR_NOHANDLER Op=Add El1=SK_SKIPPED_DONE El2=C_1 Exit=SK_SKIPPED_DONE
NOVA LOAD_CUR_HCONT Op=Move El1=C_0 Exit=RA_CU_CONTINUE
NOVA LOAD_CUR_NHJ Op=Jump Exit=LOAD_CU_SKPL

── LOAD_CU_PREPARE: WAX_CMD-bypass / declare-only gate / generic arg pre-read ─
//
IN:  SK_CMD_ADDR, SK_TMP (handler addr, from LOAD_CU_RESOLVE), SK_DECLARE_ONLY.
OUT: on success -- SK_TMP2 = ready for the orchestrator's Voca,
     RA_CU_CONTINUE = 1.
     on failure (declare-only gate) -- RA_CU_CONTINUE = 0; same
     already-skipped convention as LOAD_CU_RESOLVE.

Declare-only pass (Pattern Z, BUGS.md): only run commands that can
declare/allocate a name during the declare-only pass -- everything else
(a genuine use-only macro call, e.g. YAKU/WALK_ONE/CHAIN_BUILD_ALIAS)
is skipped this pass, and runs for real once every name across every
file is guaranteed to already exist.

Compares SK_CMD_ADDR against WAX_CMD's own address, computed fresh via
Idem (aspects.re, Aspect #25) right before the check, not a pre-declared
_REF holder -- Equal (_cmpeq, interpreter.py) dereferences *both*
operands, so comparing against a stale holder's *word* instead of its
address silently breaks once the holder itself is no longer self-ref.
Idem sidesteps that whole class of bug: nothing standing to go stale, at
the cost of one extra instruction per comparison. (BUGS.md Pattern Z,
ROADMAP.md D6.)

WAX_CMD is the seed command -- the one that fills the dispatch table the
rest of this dispatcher consults -- and reads its own tokens directly
(BS_READ_TOKEN), unlike everything else reaching this point, which needs
the generic MA0..MA7 pre-read (LOAD_READARG_KV) below. This check must
run before both the SK_DECLARE_ONLY gate and LOAD_READARG_KV: either one
consuming WAX_CMD's own arguments first would leave nothing for its own
read. Only the seed bypasses both; ordinary SET takes the generic path
like everything else.
//
NOLINK
NOVA LOAD_CU_PREPARE Op=Idem El1=WAX_CMD Exit=SK_TMP3
JEQ LOAD_CU_SEEDCK0 El1=SK_CMD_ADDR El2=SK_TMP3 Exit=LOAD_CU_SEEDSK
JZ LOAD_CU_DOCK El1=SK_DECLARE_ONLY Exit=LOAD_CU_BME1S
/Declare-only allowlist: commands that create real, named content and so
/must still run during the declare pass. Membership is the table below,
/walked by this loop -- it used to be two instructions per command
/(fetch own address, compare, jump) written out eleven times down the
/file, which meant reading a chain of jumps to find out who was in it
/and inserting a pair in the right spot to add one. WAX_CMD is
/deliberately not in the table: it is checked above, before this gate,
/because it reads its own tokens rather than taking the generic
/MA0..MA7 pre-read.
NOVA LOAD_CU_DINIT Op=Move El1=SK_DECL_TABLE_BASE Exit=SK_DECL_PTR
NOVA LOAD_CU_DREAD Op=Read El1=SK_DECL_PTR Exit=SK_TMP3
/Terminator (SK_TMP3==0): command is NOT in the declare allowlist, so this
/declare-only pass must SKIP it. Jump to LOAD_CU_DSKCONT (which clears
/RA_CU_CONTINUE first), not straight to LOAD_CU_DSKJ -- otherwise the skip
/leaves RA_CU_CONTINUE at 1 (set by LOAD_CU_RESOLVE's CCOUNT path just before),
/and the caller (LOAD_CMD_UNKNOWN's LOAD_CU_PCK) then falls through to
/LOAD_CU_BCALL and Voca's a stale SK_TMP2 into unrelated code. Latent until
/command words became resolvable (SK_HTAB seed): before, an unrecognised command
/never reached this gate at all, so DSKCONT was dead code and the miss path was
/never exercised.
JZ LOAD_CU_DEND El1=SK_TMP3 Exit=LOAD_CU_DSKCONT
JEQ LOAD_CU_DHIT El1=SK_CMD_ADDR El2=SK_TMP3 Exit=LOAD_CU_BME1S
NOVA LOAD_CU_DNEXT Op=Add El1=SK_DECL_PTR El2=C_1 Exit=SK_DECL_PTR
NOVA LOAD_CU_DLOOP Op=Jump Exit=LOAD_CU_DREAD
NOLINK
NOVA LOAD_CU_DSKCONT Op=Move El1=C_0 Exit=RA_CU_CONTINUE
NOVA LOAD_CU_DSKJ Op=Jump Exit=LOAD_CU_SKPL
NOLINK
/WAX_CMD reads its own tokens directly (BS_READ_TOKEN, native-style),
/unlike every other command reaching this point (YAKU/WALK_ONE/SWITCH/
/FOR/CHAIN/SAVE/CLEAR/NOP/JEQ/JZ/ALLOC_TO, all "builder macro" style,
/needing MA0..MA7 pre-read from the file via LOAD_READARG_KV). Running
/LOAD_READARG_KV unconditionally before every dispatch would consume
/WAX_CMD's own tokens first, leaving its own read (further down) nothing
/to find -- the same reason WAX_CMD's check above must run before this
/generic pre-read at all.
/
/JEQ/JZ/WALK_ONE/LH/LX in particular are *dual-purpose*: also called
/internally via plain RVOCA from other macro bodies (e.g. EXP_SWITCH_START's
/own "RVOCA EXP_SWITCH_E1_JEQ JEQ"), with MA0..MA7 already set directly by
/the caller, no file reading involved at all -- moving LOAD_READARG_KV
/inside each of *those* handlers (an earlier draft of this fix) would
/have broken every such internal call. Keeping the pre-read here, at
/the dispatcher, with one explicit, direct exception for WAX_CMD
/specifically, avoids that entirely -- mirrors this dispatcher's own
/established style for special cases (compare the CLEAR/NOP/JEQ/JZ/
/ALLOC_TO checks above).
NOVA LOAD_CU_BME1S Op=Move El1=SK_TMP Exit=SK_TMP2
RVOCA LOAD_CU_MAS Body=LOAD_READARG_KV
JZ LOAD_CU_MA0CK El1=RA_MA0 Exit=LOAD_CU_MA0SK
/Auto-complete _J/_K for JEQ and JZ macros
JEQ LOAD_CU_JZCK El1=SK_TMP2 El2=JZ Exit=LOAD_CU_JZ_PATH
JEQ LOAD_CU_JEQCK El1=SK_TMP2 El2=JEQ Exit=LOAD_CU_JEQ_PATH
NOVA LOAD_CU_JK_SKIP Op=Jump Exit=LOAD_CU_PREP_DONE
NOLINK
RVOCA LOAD_CU_JEQ_PATH Body=LOAD_MA_JEQ_J
NOVA LOAD_CU_JK_DONE Op=Jump Exit=LOAD_CU_PREP_DONE
NOLINK
RVOCA LOAD_CU_JZ_PATH Body=LOAD_MA_JZ_J
NOVA LOAD_CU_JK_DONE2 Op=Jump Exit=LOAD_CU_PREP_DONE
NOLINK
/WAX_CMD: set SK_TMP2 (needed by the orchestrator's Voca) but skip the
/argument pre-read entirely -- WAX_CMD reads its own tokens directly.
NOVA LOAD_CU_SEEDSK Op=Move El1=SK_TMP Exit=SK_TMP2
NOVA LOAD_CU_SEEDJ Op=Jump Exit=LOAD_CU_PREP_DONE
NOLINK
NOVA LOAD_CU_MA0SK Op=Move El1=C_0 Exit=RA_CU_CONTINUE
NOVA LOAD_CU_MA0SKJ Op=Jump Exit=LOAD_CU_SKPL
NOLINK
NOVA LOAD_CU_PREP_DONE Op=Move El1=C_1 Exit=RA_CU_CONTINUE
RREDI LOAD_CU_PREP_RET

── LOAD_CU_SKPL: skip to end of line, shared exit for both pieces above ─
NOLINK
RVOCA LOAD_CU_SKPL Body=BS_SKIP_TO_EOL
RREDI LOAD_CU_SKRRET

── LOAD_READARG_KV: unified arg reader ───────────────────────
//
Single function for reading macro arguments from file.
Handles: key=value (any discovered key -> SK_ARG_<key>, Name -> MA0),
integer literals → C_N addr, and positional tokens → MA0..MA7 in order:
the FIRST keyless token lands in MA0 (it is the name -- "first token
without a key == Name"), the rest in MA1..MA7. This is exact parity with
_split_args_slots' own args.insert(0, name): the name always occupies
position 0 (MA0) whether written as the first keyless token or as Name=,
and every other positional shifts into 1+ (MA1+). Uses the shared
LOAD_ELEM for classification instead of its own byte tree.
//
NOLINK
NOVA LOAD_READARG_KV Op=Move El1=C_1 Exit=SK_POS_SLOT
/Clear MA0..MA7 and the dedicated key registers
FOR RA_MA0 RA_MA1 RA_MA2 RA_MA3 RA_MA4 RA_MA5 RA_MA6 RA_MA7
    CLEAR LOAD_RKVA_CLR{N} El1={X}

AUTOGEN_KEY_CLEARS LOAD_RKVA_CLR
NOVA LOAD_RKVA_LOOPJ Op=Jump Exit=LOAD_RKVA_LOOP
NOLINK
JEQ LOAD_RKVA_LOOP El1=RA_LOAD_BYTE El2=LF Exit=LOAD_RKVA_DONE
JZ LOAD_RKVA_EOFCK El1=RA_LOAD_BYTE Exit=LOAD_RKVA_DONE
RVOCA LOAD_RKVA_TOK Body=BS_READ_TOKEN
JZ LOAD_RKVA_EMCK El1=RA_LOAD_TLEN Exit=LOAD_RKVA_DONE
RVOCA LOAD_RKVA_CLS Body=LOAD_ELEM
/Route by SK_ELEM_KEY, the numeric code LOAD_ELEM's HT_LOOKUP returned:
/0=positional, otherwise the keyed store below indexes KEY_DEST[code]. The
/name needs no key of its own -- it is just the first keyless token, which
/the positional path lands in MA0 (SK_POS_SLOT starts at 1); every later
/positional token goes to MA1+.
JZ LOAD_RKVA_PCK0 El1=SK_ELEM_KEY Exit=LOAD_RKVA_PSTART
/Keyed: one generic store for every key, replacing the per-key JEQ chain +
/N store blocks. dest = KEY_DEST[code] (= &SK_ARG_<key>, its 2-wide slot);
/resolve the value; write value at [dest] and its kind at [dest+1]. dest is
/held in SK_KDEST across LOAD_ELEM_RESOLVE (which clobbers SK_TMP). Forward
/references backfill into SK_ELEM_EXIT (= dest), exactly as before.
NOVA LOAD_RKVA_KBASE Op=Idem El1=KEY_DEST Exit=SK_TMP
NOVA LOAD_RKVA_KADDR Op=Add El1=SK_TMP El2=SK_ELEM_KEY Exit=SK_KDEST
NOVA LOAD_RKVA_KDST Op=Read El1=SK_KDEST Exit=SK_KDEST
NOVA LOAD_RKVA_KEXIT Op=Move El1=SK_KDEST Exit=SK_ELEM_EXIT
RVOCA LOAD_RKVA_KRS Body=LOAD_ELEM_RESOLVE
NOVA LOAD_RKVA_KW Op=Write El1=SK_KDEST El2=SK_ELEM_VALUE
NOVA LOAD_RKVA_KKA Op=Add El1=SK_KDEST El2=C_1 Exit=SK_TMP
NOVA LOAD_RKVA_KKW Op=Write El1=SK_TMP El2=SK_ELEM_KIND
NOVA LOAD_RKVA_KJ Op=Jump Exit=LOAD_RKVA_LOOP
NOLINK
/Positional: resolve then write to MA[pos_slot]
NOLINK
//
Compute the real destination address into SK_ELEM_EXIT (via Idem,
Aspect #25) *before* calling resolve, mirroring the switch below --
SK_POS_SLOT itself is untouched by LOAD_ELEM_RESOLVE/LOAD_INTERN (only
read/written by this switch and the increment steps), so reading it
twice, once before and once after the call, is safe; nothing else
about it needs saving the way the El1-vs-El2 case above needed SK_TMP2.
A forward-referenced positional argument now backfills straight into
its real RA_MAx slot instead of the too-late SK_ELEM_VALUE. See BUGS.md
Pattern Z.

LOAD_RKVA_PSTART exists because this switch is anonymous and so cannot
be jumped to by name, and its only physical predecessor is an
unconditional Jump (LOAD_RKVA_C4J), which always leaves via Exit and
never falls through. The switch was therefore unreachable: every
positional path jumped straight to LOAD_RKVA_POS, which sits *after*
it, so SK_ELEM_EXIT was never set for a positional argument and a
forward-referenced one backfilled into whatever address the previous
operation had left there. A named no-op entry, adjacent to the switch,
restores the intended route -- every positional path now enters here.
//
NOP LOAD_RKVA_PSTART
SWITCH SK_POS_SLOT
    1 LOAD_RKVA_PSI0
    2 LOAD_RKVA_PSI1
    3 LOAD_RKVA_PSI2
    4 LOAD_RKVA_PSI3
    5 LOAD_RKVA_PSI4
    6 LOAD_RKVA_PSI5
    7 LOAD_RKVA_PSI6
    8 LOAD_RKVA_PSI7

NOVA LOAD_RKVA_PSIFALL Op=Jump Exit=LOAD_RKVA_POS
NOLINK
FOR RA_MA0 RA_MA1 RA_MA2 RA_MA3 RA_MA4 RA_MA5 RA_MA6
    NOLINK
    NOVA LOAD_RKVA_PSI{N} Op=Idem El1={X} Exit=SK_ELEM_EXIT
    NOVA LOAD_RKVA_PSI{N}J Op=Jump Exit=LOAD_RKVA_POS

NOLINK
NOVA LOAD_RKVA_PSI7 Op=Idem El1=RA_MA7 Exit=SK_ELEM_EXIT
RVOCA LOAD_RKVA_POS Body=LOAD_ELEM_RESOLVE
SWITCH SK_POS_SLOT
    1 LOAD_RKVA_W0
    2 LOAD_RKVA_WP0
    3 LOAD_RKVA_WP1
    4 LOAD_RKVA_WP2
    5 LOAD_RKVA_WP3
    6 LOAD_RKVA_WP4
    7 LOAD_RKVA_WP5
    8 LOAD_RKVA_WP6

NOVA LOAD_RKVA_FALL Op=Jump Exit=LOAD_RKVA_LOOP
/Positional slot 0 (MA0): write and save hash for _J/_K extension
NOLINK
NOVA LOAD_RKVA_W0 Op=Move El1=SK_ELEM_VALUE Exit=RA_MA0
NOVA LOAD_RKVA_W0_SH Op=Move El1=SK_ELEM_HASH Exit=SK_MA_HASH0
NOVA LOAD_RKVA_INC0 Op=Add El1=SK_POS_SLOT El2=C_1 Exit=SK_POS_SLOT
NOVA LOAD_RKVA_J0 Op=Jump Exit=LOAD_RKVA_LOOP
FOR RA_MA1 RA_MA2 RA_MA3 RA_MA4 RA_MA5 RA_MA6 RA_MA7
    NOLINK
    NOVA LOAD_RKVA_WP{N} Op=Move El1=SK_ELEM_VALUE Exit={X}
    NOVA LOAD_RKVA_INCP{N} Op=Add El1=SK_POS_SLOT El2=C_1 Exit=SK_POS_SLOT
    NOVA LOAD_RKVA_JP{N} Op=Jump Exit=LOAD_RKVA_LOOP

NOLINK
RREDI LOAD_RKVA_DONE

── LOAD_ELEM_RESOLVE: resolve current tokbuf → SK_ELEM_VALUE ──
//
Handles: integer ('-' or digit) → interned constant-cell addr (BS_INTERN,
value written into the cell), symbol → LOAD_INTERN result.
Sets SK_ELEM_VALUE and SK_ELEM_HASH.
CALLER CONTRACT: for the Symbol path, the caller must set SK_ELEM_EXIT
(via Idem, Aspect #25) to its own real, final destination address
*before* calling this -- not to SK_ELEM_VALUE's own address. SK_ELEM_VALUE
is only an intermediate register; every real caller copies it into its
own destination via a separate Move step immediately after this
returns, so backfilling SK_ELEM_VALUE itself would be too late (that
copy has already happened with the necessarily-wrong immediate 0 by
the time backfill could run). Was plain BS_LOOKUP (no backfill at all)
until this session -- silently broke on any forward reference to a
not-yet-declared name, the same class of bug fixed for SEAL specifically
earlier this session, just in the single most widely-shared resolver in
the whole self-hosted graph. See BUGS.md Pattern Z.
//
NOLINK
NOVA LOAD_ELEM_RESOLVE Op=Read El1=BS_TOKBUF_BASE Exit=SK_ELEM_VALUE
/Default kind = 0 (name or string: SK_ELEM_VALUE is the address to store).
/The integer path below overrides it to 1.
CLEAR LOAD_RKVR_KIND0 El1=SK_ELEM_KIND
/String literal? First byte is a double-quote. The quote-aware tokenizer
/keeps "a b c" (spaces and both quotes) as one token, so packing it here
/turns a string into an ordinary resolvable value -- its chain address --
/usable by any command, exactly like an int or a name.
JEQ LOAD_RKVR_STRCK El1=SK_ELEM_VALUE El2=DQUOTE Exit=LOAD_RKVR_STR
/Integer?
NOVA LOAD_RKVR_DIGS Op=Sub El1=SK_ELEM_VALUE El2=ASCII_0 Exit=SK_TMP
NOVA LOAD_RKVR_DIGCK Op=ULess El1=SK_TMP El2=C_10 Exit=SK_FLAG
NOVA LOAD_RKVR_DIGCJ Op=JumpIf El1=SK_FLAG Exit=LOAD_RKVR_INT
JEQ LOAD_RKVR_NEGCK El1=SK_ELEM_VALUE El2=MINUS Exit=LOAD_RKVR_INT
/Symbol -- LOAD_INTERN, not BS_LOOKUP, so a forward reference defers
/(via backfill into the caller's own pre-set SK_ELEM_EXIT) instead
/of silently resolving to 0/garbage.
RVOCA LOAD_RKVR_LK Body=LOAD_INTERN
NOVA LOAD_RKVR_LKRES Op=Move El1=RA_BS_RESULT Exit=SK_ELEM_VALUE
NOVA LOAD_RKVR_SVHASH Op=Move El1=RA_LOAD_HASH Exit=SK_ELEM_HASH
RREDI LOAD_RKVR_RRET
/Integer literal → find-or-mint a constant cell holding the value; the
/resolved SK_ELEM_VALUE is that cell's ADDRESS. This mirrors loader.py
/_resolve's C_<v> path exactly: an integer operand never resolves to the
/raw value (every NOVA field is read through a dereference, aether[slot]),
/it resolves to the address of a cell holding the value, and repeated uses
/of the same literal share one cell.
/
/An integer is interned the same way a symbol name is: BS_PARSE_INT leaves
/the decimal token in the buffer with its hash intact (RA_LOAD_HASH,
/RA_LOAD_TLEN), and a decimal key can never collide with a real name (those
/all start with a letter or '_'). BS_INTERN find-or-allocs a 1-lux entry
/keyed by that token and self-refs it on a miss; the Write below overrides
/that word with the actual number -- idempotent on a re-use hit, since the
/cell already holds the same value. Was addr(C_0)+value*2 until this
/session, which assumed C_0..C_N were one contiguous stride-2 array; they
/are not (each C_<v> is allocated lazily wherever the bump allocator lands
/it), so every integer operand resolved to a wrong, unrelated address. The
/reader never ran end to end, so it stayed a latent, never-exercised bug.
NOLINK
RVOCA LOAD_RKVR_INT Body=BS_PARSE_INT
RVOCA LOAD_RKVR_CI Body=BS_INTERN
NOVA LOAD_RKVR_CVAL Op=Write El1=RA_BS_RESULT El2=RA_BS_PIVAL
NOVA LOAD_RKVR_CADDR Op=Move El1=RA_BS_RESULT Exit=SK_ELEM_VALUE
NOVA LOAD_RKVR_SVHI Op=Move El1=RA_LOAD_HASH Exit=SK_ELEM_HASH
/Kind = 1: this is a number literal. A consumer that wants the NUMBER
/(SET/Count/Width) reads it by dereferencing SK_ELEM_VALUE (= the cell's
/address); an operand consumer uses the address directly, as before.
NOVA LOAD_RKVR_KIND1 Op=Move El1=C_1 Exit=SK_ELEM_KIND
RREDI LOAD_RKVR_INTRR
/String → pack the "..." token into a lux chain; SK_ELEM_VALUE = chain
/address (kind stays 0). LOAD_PACK_STRING reads the bytes straight from
/BS_TOKBUF, skipping the surrounding quotes.
NOLINK
RVOCA LOAD_RKVR_STR Body=LOAD_PACK_STRING
NOVA LOAD_RKVR_STRRES Op=Move El1=PGB_PS_FIRST Exit=SK_ELEM_VALUE
NOVA LOAD_RKVR_STRHASH Op=Move El1=RA_LOAD_HASH Exit=SK_ELEM_HASH
RREDI LOAD_RKVR_STRRR

/── LOAD_PACK_STRING: pack a "..." tokbuf literal into a lux chain ──
//
IN:  BS_TOKBUF holds a double-quoted token, RA_LOAD_TLEN = its length
     INCLUDING both quotes.
OUT: PGB_PS_FIRST = address of the first packed lux (NUL-terminated chain).
Reads bytes [1 .. tlen-2] (the content between the quotes), unescaping
\n -> LF and \t -> TAB, and hands each to PGB_EMIT_BYTE -- the same
packer SET's own former stream loop used, now driven from the buffer so
a string that arrived as one quote-aware token (e.g. Value="a b") packs
without re-reading the stream. PGB_FINALIZE flushes + terminates.
//
NOLINK
CLEAR LOAD_PACK_STRING El1=PGB_PS_WORD
CLEAR LOAD_PS_CS El1=PGB_PS_SHIFT
CLEAR LOAD_PS_CF El1=PGB_PS_FIRST
CLEAR LOAD_PS_CP El1=PGB_PS_PREV
NOVA LOAD_PS_I0 Op=Move El1=C_1 Exit=SK_PS_I
NOVA LOAD_PS_LIM0 Op=Sub El1=RA_LOAD_TLEN El2=C_1 Exit=SK_PS_LIM
NOVA LOAD_PS_LOOP Op=ULess El1=SK_PS_I El2=SK_PS_LIM Exit=SK_FLAG
NOVA LOAD_PS_LOOPJ Op=JumpIf El1=SK_FLAG Exit=LOAD_PS_RD
NOVA LOAD_PS_LOOPX Op=Jump Exit=LOAD_PS_FIN
NOLINK
NOVA LOAD_PS_RD Op=Add El1=BS_TOKBUF_BASE El2=SK_PS_I Exit=SK_PS_ADDR
NOVA LOAD_PS_RDB Op=Read El1=SK_PS_ADDR Exit=RA_TMP
/escape? a backslash consumes the next byte and maps n->LF, t->TAB
JEQ LOAD_PS_BSCK El1=RA_TMP El2=BACKSLASH Exit=LOAD_PS_ESC
NOVA LOAD_PS_EMITJ Op=Jump Exit=LOAD_PS_EMIT
NOLINK
NOVA LOAD_PS_ESC Op=Add El1=SK_PS_I El2=C_1 Exit=SK_PS_I
NOVA LOAD_PS_ESCA Op=Add El1=BS_TOKBUF_BASE El2=SK_PS_I Exit=SK_PS_ADDR
NOVA LOAD_PS_ESCRD Op=Read El1=SK_PS_ADDR Exit=RA_TMP
JEQ LOAD_PS_ESCN El1=RA_TMP El2=ASCII_nl Exit=LOAD_PS_SETLF
JEQ LOAD_PS_ESCT El1=RA_TMP El2=ASCII_tl Exit=LOAD_PS_SETTAB
NOVA LOAD_PS_ESCLIT Op=Jump Exit=LOAD_PS_EMIT
NOLINK
NOVA LOAD_PS_SETLF Op=Move El1=LF Exit=RA_TMP
NOVA LOAD_PS_SETLFJ Op=Jump Exit=LOAD_PS_EMIT
NOLINK
NOVA LOAD_PS_SETTAB Op=Move El1=TAB Exit=RA_TMP
NOVA LOAD_PS_SETTABJ Op=Jump Exit=LOAD_PS_EMIT
NOLINK
RVOCA LOAD_PS_EMIT Body=PGB_EMIT_BYTE
NOVA LOAD_PS_INC Op=Add El1=SK_PS_I El2=C_1 Exit=SK_PS_I
NOVA LOAD_PS_J Op=Jump Exit=LOAD_PS_LOOP
NOLINK
RVOCA LOAD_PS_FIN Body=PGB_FINALIZE
RREDI LOAD_PS_RET

//
── LOAD_MA_AUTOCOMPLETE_J: shared body for JEQ/JZ's own "_J" lookup ──
IN:  RA_MC_JTARGET = address of the register to check/fill (RA_MA4 for
                     JEQ, RA_MA3 for JZ -- caller passes its own
                     register's address via Idem, then this reads/
                     writes through it)
     SK_MA_HASH0   = hash of the base macro name (JEQ or JZ), already
                     set by the caller before calling this
If the target register already holds a nonzero value, done (already
resolved, matching every caller's own pre-check). Otherwise: hash '_J'
onto SK_MA_HASH0, look it up; if found, use that address; if not,
allocate a fresh NOVA block. Either way, write the result through
*JTARGET.

Found identical in shape (hash '_J' onto the base name's hash, look
up, allocate if missing) between JEQ's and JZ's own versions, differing
only in which register held the result and which scratch register
carried the hash mid-computation (RA_BS_TMP2 vs reusing RA_LOAD_HASH
directly -- both correct, arbitrary style, not a real distinction).
Plain runtime code (Read/Write through a computed address), not
NOVAE, so unlike several macros in synth.re this has no Op1/Op2/Op3
literal-text restriction blocking a real merge.
//
NOLINK
NOVA LOAD_MA_AUTOCOMPLETE_J Op=Read El1=RA_MC_JTARGET Exit=SK_TMP3
JZ LOAD_MAJ_PRECK El1=SK_TMP3 Exit=LOAD_MAJ_DO_J
RREDI LOAD_MAJ_RRET
NOLINK
HASH_STEP LOAD_MAJ_DO_J SK_MA_HASH0 UNDERSCORE RA_BS_TMP2 RA_BS_TMP2
HASH_STEP LOAD_MAJ_JJ1  RA_BS_TMP2  ASCII_J    RA_LOAD_HASH RA_LOAD_HASH
RVOCA LOAD_MAJ_JLK Body=BS_LOOKUP
NOVA LOAD_MAJ_GOTR Op=Move El1=RA_BS_RESULT Exit=SK_TMP3
JZ LOAD_MAJ_JCK El1=SK_TMP3 Exit=LOAD_MAJ_JALLOC
NOVA LOAD_MAJ_STORE Op=Write El1=RA_MC_JTARGET El2=SK_TMP3
RREDI LOAD_MAJ_DONE

NOLINK
NOVA LOAD_MAJ_JALLOC Op=Move El1=NOVA_SIZE Exit=RA_ALLOC_COUNT
RVOCA LOAD_MAJ_ALC Body=ALLOC
NOVA LOAD_MAJ_ASTORE Op=Write El1=RA_MC_JTARGET El2=RA_ALLOC_RESULT
RREDI LOAD_MAJ_ADONE

── LOAD_MA_JEQ_J: auto-complete MA4 (_J) for JEQ ────────────
//
JEQ Name=macro no longer needs a _K suffix (NOP removed — JumpIf's own
slot 5 serves as the fall-through point). Only _J needs resolving.
Thin wrapper over the shared LOAD_MA_AUTOCOMPLETE_J -- see its own
docstring above for why this and JZ's version were merged.
//
NOLINK
NOVA LOAD_MA_JEQ_J Op=Idem El1=RA_MA4 Exit=RA_MC_JTARGET
RVOCA LOAD_MAJ_CALL Body=LOAD_MA_AUTOCOMPLETE_J
RREDI LOAD_MAJ_WRET

── LOAD_MA_JZ_J: auto-complete MA3 (_J) for JZ ──────────────
/JZ macro no longer needs a _K suffix (same reasoning as JEQ above).
/Thin wrapper over the shared LOAD_MA_AUTOCOMPLETE_J.
NOLINK
NOVA LOAD_MA_JZ_J Op=Idem El1=RA_MA3 Exit=RA_MC_JTARGET
RVOCA LOAD_MZJ_CALL Body=LOAD_MA_AUTOCOMPLETE_J
RREDI LOAD_MZJ_WRET

── LOAD_POOL_NAME removed ─────────────────────────────────────
/It copied a forward-referenced name's bytes into a private pool
/(SK_SEAL_NAMEPOOL) so backfill could re-look-it-up later by hash+name.
/That whole detour is gone: a forward reference now records the address
/of the name's own HT entry (BS_ENTRY_ADDR, intern.re) instead of a name
/copy, and the resolve pass reads the located lid straight out of that
/entry -- no re-lookup, so nothing needs the name text kept around. This
/also removed the SK_SEAL_NAMEPOOL buffer and its unbounded growth (a
/pool sized 16384 that was never reset overran into its own pointer and
/then into low memory / the call stack -- the file-29 self-hosting halt).

── LOAD_INTERN: intern with backfill support ─────────────────
//
If found: return addr in RA_BS_RESULT.
If NOT found: record (SK_ELEM_EXIT, hash) in backfill, return 0.
Use BS_INTERN for names that must exist; LOAD_INTERN for forward refs.
//
NOLINK
RVOCA LOAD_INTERN Body=BS_LOOKUP
JZ LOAD_INT_CK El1=RA_BS_RESULT Exit=LOAD_INT_MISS
RREDI LOAD_INT_RRET
/Miss = the name has no location yet (forward reference). Defer, do not
/pre-allocate: the referenced name's eventual size is unknown here, and a
/wrong-sized eager stub would poison its real declaration (PS_CMD_NEW
/reuses an existing entry in place). So record only two words --
/(dest_slot, name's HT-entry address) -- and let the resolve pass read
/the located lid out of that entry once the declaration has upserted it.
/BS_ENTRY_ADDR (intern.re) find-or-creates the entry (unlocated, lid 0)
/and returns its stable word0 address in RA_BS_ENTRY. No name copy, no
/re-lookup, no private pool -- the entry itself is the name's identity.
RVOCA LOAD_INT_MISS Body=BS_ENTRY_ADDR
NOVA Op=Write El1=SK_BACKFILL_PTR El2=SK_ELEM_EXIT
NOVA Op=Add El1=SK_BACKFILL_PTR El2=C_1 Exit=SK_BACKFILL_PTR
NOVA Op=Write El1=SK_BACKFILL_PTR El2=RA_BS_ENTRY
NOVA Op=Add El1=SK_BACKFILL_PTR El2=C_1 Exit=SK_BACKFILL_PTR
/Sentinel written but not advanced past: the next record overwrites it.
NOVA Op=Write El1=SK_BACKFILL_PTR El2=C_0
CLEAR LOAD_INT_ZERO El1=RA_BS_RESULT
RREDI LOAD_INT_MRRET

/LOAD_CN_INTERN removed: dead code, zero real callers anywhere in the
/project. It existed for LOAD_CMD_SEAL, which was itself already
/removed (SEAL retired as a pure subset of SET -- see loader.py and
/expand.re's own note on LOAD_CMD_SEAL) without this supporting piece
/being cleaned up alongside it.

── SK_IND_DEPTH declaration ──────────────────────────────────
/Two-valued body marker published by LOAD_INDENT_MARK (0=none, 1=one
/level, 2=two+ levels). The reader and classifier that produce it are
/LOAD_READ_LINE / LOAD_INDENT_CLASSIFY / LOAD_INDENT_MARK, all further
/down this file -- this is only where the register itself is declared.
NEW SK_IND_DEPTH      /0=none, 1=one level, 2=two+ levels
/EXP_CMD_FOR_IMPL's own scratch registers (header parsing, override
/resolution). See BUGS.md Pattern Z.
NEW SK_CF_HIDX In=OWN  /head-of-list reset loop index
NEW SK_CF_NIDX In=OWN  /is-this-token-numeric scan index
NEW SK_CF_ISNUM In=OWN  /1 while every byte scanned so far is a digit
NEW SK_CF_AIDX In=OWN  /anonymous-count expansion loop index
NEW SK_CF_CIDX In=OWN  /named-slot byte-copy loop index
/Override-table parser (EXP_CF_PARSE_OVERRIDES) scratch registers.
NEW SK_CF_OV_PASS In=OWN  /current priority pass: 0=default, 1=pos/range, 2=name
NEW SK_CF_OV_PTR In=OWN  /walk pointer through SK_BODY_BASE
NEW SK_CF_OV_LSTART In=OWN  /current line's own content start (after marker)
NEW SK_CF_OV_LLEN In=OWN  /current line's own content length (excl. LF)
NEW SK_CF_OV_GTPOS In=OWN  /position of '>' within the line, -1 (C_MAXU) if none
NEW SK_CF_OV_SEL_LEN In=OWN  /selector text length (bytes before '>')
NEW SK_CF_OV_PRI In=OWN  /this line's own priority tier
NEW SK_CF_OV_SLOTLO In=OWN  /resolved slot range: low (inclusive)
NEW SK_CF_OV_SLOTHI In=OWN  /resolved slot range: high (inclusive)
NEW SK_CF_OV_KVPOS In=OWN  /walk position within the key=value pairs text
NEW SK_CF_OV_SLOT In=OWN  /current slot being written to, in the apply loop
NEW SK_CF_OV_FOUND In=OWN  /1 if EXP_CF_OV_FIND_NAME_SLOT resolved a slot
NEW SK_CF_OV_KVEND In=OWN  /end address of the key=value text for this line
NEW SK_CF_OV_KSTART In=OWN  /current pair's key start address
NEW SK_CF_OV_EQPOS In=OWN  /address of '=' within the current pair (also
                             /reused for dash-position within CLASSIFY_SEL)
NEW SK_CF_OV_VEND In=OWN  /current pair's value end address
NEW SK_CF_OV_KLEN In=OWN  /current pair's key length
NEW SK_CF_OV_VSTART In=OWN  /current pair's value start address
NEW SK_CF_OV_VLEN In=OWN  /current pair's value length
NEW SK_CF_OV_SLOTHIP1 In=OWN  /SK_CF_OV_SLOTHI + 1 (loop bound, exclusive)
NEW SK_CF_OV_HDADDR In=OWN  /a slot's own head-pointer address (EXP_CF_OV_MAKE_NODE scratch)
NEW SK_CF_UI_ADDR In=OWN  /EXP_CF_PARSE_UINT: start address to parse
NEW SK_CF_UI_LEN In=OWN  /EXP_CF_PARSE_UINT: byte count to parse
NEW RA_FOR_IRIS_BASE_ADDR  /RA_FOR_IRIS_BUF's own real address
                                    /(SPAN buffers don't self-ref -- see
                                    /BUGS.md Pattern Z -- so this is set
                                    /once via Idem, reused for every
                                    /offset computation instead of
                                    /re-dereferencing the raw symbol)
NEW RA_FOR_IRIS_IDX_BASE_ADDR  /RA_FOR_IRIS_IDX_BASE's own real address
NEW RA_FOR_OV_HEAD_BASE_ADDR  /RA_FOR_OV_HEAD_BASE's own real address

── LOAD_READ_LINE: exact-indent line reader ──
//
Consumes the next line's leading indent, counting it EXACTLY into
SK_IND_COUNT (SP=+1, TAB=+4, stopping at the first byte that is
neither). Sets RA_LOAD_BYTE (0 = EOF); on return RA_LOAD_BYTE holds
the line's first real content byte, and the caller reads the rest of
the line itself.

This replaced an earlier implementation that collapsed anything past
two indent levels into "2+" and could not tell 8 spaces from 12. The
exact count is what lets LOAD_INDENT_CLASSIFY compare a line against
the level stack, which is how returning to an already-seen shallower
level is recognised at all.

Callers wanting the derived level and body marker call
LOAD_INDENT_MARK, which classifies this count and publishes both
RA_LOAD_INDENT (exact level) and SK_IND_DEPTH (two-valued marker).

Note this CONSUMES the indent bytes -- it is for callers walking a
body line by line. LOAD_COUNT_INDENT measures the same thing without
consuming, for the dispatcher, which must not move the read position.
//
SET LOAD_READ_LINE Value=LOAD_RL_IMPL
NOLINK
CLEAR LOAD_RL_IMPL El1=SK_IND_COUNT
RVOCA LOAD_RL_LOOP Body=BS_READ_BYTE
JZ LOAD_RL_EOFCK El1=RA_LOAD_BYTE Exit=LOAD_RL_DONE
/JEQ jumps to its dest ON MATCH, so each dest is the matching case.
JEQ LOAD_RL_SPCK El1=RA_LOAD_BYTE El2=SP Exit=LOAD_RL_SP
JEQ LOAD_RL_TABCK El1=RA_LOAD_BYTE El2=TAB Exit=LOAD_RL_TAB
/Neither SP nor TAB: this byte is the line's first real content byte.
/It stays in RA_LOAD_BYTE for the caller, which continues reading the
/rest of the line itself. Note there is deliberately no un-read here:
/decrementing RA_LOAD_RPOS would be wrong under redirect mode, where
/BS_READ_BYTE advances RA_REDIRECT_POS and leaves RA_LOAD_RPOS alone.
NOVA LOAD_RL_ENDJ Op=Jump Exit=LOAD_RL_DONE
NOLINK
NOVA LOAD_RL_SP Op=Add El1=SK_IND_COUNT El2=C_1 Exit=SK_IND_COUNT
NOVA LOAD_RL_SPJ Op=Jump Exit=LOAD_RL_LOOP
NOLINK
NOVA LOAD_RL_TAB Op=Add El1=SK_IND_COUNT El2=C_4 Exit=SK_IND_COUNT
NOVA LOAD_RL_TABJ Op=Jump Exit=LOAD_RL_LOOP
NOLINK
RREDI LOAD_RL_DONE

── LOAD_INDENT_CLASSIFY: exact indent-stack classifier ──
//
Mirrors indent_stack.py's IndentStack.classify() exactly (verified
there first, in isolation, with test_indent_stack.py, before this port
-- see that file for the rule itself and its worked examples). Wired
(the "(new, unwired)" this comment used to say was stale -- LOAD_
INDENT_MARK, below, already calls this directly; found and fixed
while checking for real duplicates elsewhere in this file).

IN:  SK_IND_COUNT (this line's exact indent, from LOAD_READ_LINE)
     SK_IND_TOP    (index of current deepest known level)
     SK_IND_STACK_BASE[0..SK_IND_TOP] (the known level values)
OUT: SK_IND_ACTION -- 0=skip 1=same 2=deeper 3=return
     SK_IND_LEVEL  -- matched/pushed level index (meaningful iff ACTION != 0)
     May update SK_IND_TOP and SK_IND_STACK contents (push on 'deeper',
     truncate on 'return').

Level 0 (the opener's own indent) is ALWAYS normalized to 'skip' --
confirmed with the project author directly: a line landing back on the
opener's own indent is inert, identical to a stray/garbage indent, not
a meaningful return. Callers never need a special case for level==0.
//
── LOAD_COUNT_INDENT: measure this line's exact leading indent ──
//
Counts leading SP(+1)/TAB(+4) into SK_IND_COUNT, stopping at the first
byte that is neither. Called at line start (from LOAD_DISPATCH_LINE),
while RA_LOAD_RPOS still points at the line's first byte.

Reads the buffer directly via BS_READBUF_BASE + offset -- exactly how
BS_READ_BYTE itself addresses it -- and never advances RA_LOAD_RPOS, so
it consumes nothing and the caller's stream position is untouched.
Because it neither consumes nor triggers a refill, and runs while the
position is still valid, there is no window in which a refill could
invalidate what it reads. (An earlier version saved the position and
counted back over it later, from inside the command handlers; that had
exactly such a window -- a refill during the first token's read resets
RA_LOAD_RPOS to 0 and overwrites the buffer, leaving the saved offset
pointing into stale content. Counting up front removes the hole rather
than patching it.)

Bounded by RA_LOAD_RLEN: if the indent somehow runs to the end of the
valid buffer content, counting stops there instead of reading past it.

Under redirect mode (RA_REDIRECT_BASE != 0) the active source is the
redirect buffer, not BS_READBUF, and the text there is already-built
content with no meaningful source indent -- so this returns 0 without
reading anything.
//
SET LOAD_COUNT_INDENT Value=LOAD_CI_IMPL
NOLINK
CLEAR LOAD_CI_IMPL El1=SK_IND_COUNT
JZ LOAD_CI_RDCK El1=RA_REDIRECT_BASE Exit=LOAD_CI_START
RREDI LOAD_CI_DONE
NOLINK
NOVA LOAD_CI_START Op=Move El1=RA_LOAD_RPOS Exit=SK_IC_CNT_P
JEQ LOAD_CI_LOOP El1=SK_IC_CNT_P El2=RA_LOAD_RLEN Exit=LOAD_CI_DONE
NOVA LOAD_CI_ADDR Op=Add El1=BS_READBUF_BASE El2=SK_IC_CNT_P Exit=SK_TMP
NOVA LOAD_CI_RD Op=Read El1=SK_TMP Exit=SK_TMP2
JEQ LOAD_CI_SPCK El1=SK_TMP2 El2=SP Exit=LOAD_CI_SP
JEQ LOAD_CI_TABCK El1=SK_TMP2 El2=TAB Exit=LOAD_CI_TAB
NOVA LOAD_CI_ENDJ Op=Jump Exit=LOAD_CI_DONE
NOLINK
NOVA LOAD_CI_SP Op=Add El1=SK_IND_COUNT El2=C_1 Exit=SK_IND_COUNT
NOVA LOAD_CI_SPADV Op=Add El1=SK_IC_CNT_P El2=C_1 Exit=SK_IC_CNT_P
NOVA LOAD_CI_SPJ Op=Jump Exit=LOAD_CI_LOOP
NOLINK
NOVA LOAD_CI_TAB Op=Add El1=SK_IND_COUNT El2=C_4 Exit=SK_IND_COUNT
NOVA LOAD_CI_TABADV Op=Add El1=SK_IC_CNT_P El2=C_1 Exit=SK_IC_CNT_P
NOVA LOAD_CI_TABJ Op=Jump Exit=LOAD_CI_LOOP

SET LOAD_INDENT_CLASSIFY Value=LOAD_IC_IMPL
NOLINK
NOVA LOAD_IC_IMPL Op=Add El1=SK_IND_STACK_BASE El2=SK_IND_TOP Exit=SK_TMP
NOVA LOAD_IC_TOPVAL Op=Read El1=SK_TMP Exit=SK_TMP2
/JEQ jumps to its dest ON MATCH (Equal+JumpIf, confirmed directly) --
/so "indent == top" is the JUMP case here, not the fall-through.
JEQ LOAD_IC_EQCK El1=SK_IND_COUNT El2=SK_TMP2 Exit=LOAD_IC_SAME
/fell through: indent != top. Check indent > top next.
/indent > top? Equal-style test isn't a magnitude comparison, so use
/subtraction + zero-check instead: if (top + 4) == indent, deeper.
/Otherwise fall through to the "less than top" scan path -- this is
/deliberately NOT a full three-way (</==/>) magnitude compare; it only
/needs to distinguish "exactly one level deeper" from "anything else",
/and "anything else, not equal to any known level" resolves to skip
/via the scan below finding no match.
NOVA LOAD_IC_GTDIFF Op=Add El1=SK_TMP2 El2=C_4 Exit=SK_TMP
JEQ LOAD_IC_GTEQCK El1=SK_IND_COUNT El2=SK_TMP Exit=LOAD_IC_DEEPER
/fell through: indent is neither == top nor == top+4. Could still be a
/deeper level's exact value (if indent > top but not top+4 -- treated
/as skip per the "only +4 counts as deeper" rule) or a return to an
/earlier, shallower level. Scan the whole stack for an exact match.
NOVA LOAD_IC_LTPATH Op=Move El1=SK_IND_TOP Exit=SK_IC_SCAN_I
NOVA LOAD_IC_SCAN_TEST Op=Add El1=SK_IND_STACK_BASE El2=SK_IC_SCAN_I Exit=SK_TMP
NOVA LOAD_IC_SCAN_RD Op=Read El1=SK_TMP Exit=SK_TMP2
JEQ LOAD_IC_SCAN_MCK El1=SK_IND_COUNT El2=SK_TMP2 Exit=LOAD_IC_SCAN_MATCH
/fell through: this level didn't match. Try the next-shallower one.
JZ LOAD_IC_SCAN_NOMORE El1=SK_IC_SCAN_I Exit=LOAD_IC_SKIP
NOVA LOAD_IC_SCAN_NEXT Op=Sub El1=SK_IC_SCAN_I El2=C_1 Exit=SK_IC_SCAN_I
NOVA LOAD_IC_SCAN_LJ Op=Jump Exit=LOAD_IC_SCAN_TEST
NOLINK
NOVA LOAD_IC_SCAN_MATCH Op=Move El1=SK_IC_SCAN_I Exit=SK_IND_TOP
JZ LOAD_IC_SCAN_MZ El1=SK_IC_SCAN_I Exit=LOAD_IC_SKIP
NOVA LOAD_IC_SCAN_MLV Op=Move El1=SK_IC_SCAN_I Exit=SK_IND_LEVEL
NOVA LOAD_IC_SCAN_MA Op=Move El1=C_3 Exit=SK_IND_ACTION
NOVA LOAD_IC_SCAN_MJ Op=Jump Exit=LOAD_IC_DONE
NOLINK
NOVA LOAD_IC_SAME Op=Move El1=SK_IND_TOP Exit=SK_TMP2
JZ LOAD_IC_TOPZ El1=SK_IND_TOP Exit=LOAD_IC_SKIP
NOVA LOAD_IC_SAME_LV Op=Move El1=SK_IND_TOP Exit=SK_IND_LEVEL
NOVA LOAD_IC_SAME_A Op=Move El1=C_1 Exit=SK_IND_ACTION
NOVA LOAD_IC_SAME_J Op=Jump Exit=LOAD_IC_DONE
NOLINK
NOVA LOAD_IC_DEEPER Op=Add El1=SK_IND_TOP El2=C_1 Exit=SK_IND_TOP
NOVA LOAD_IC_PUSH_ADR Op=Add El1=SK_IND_STACK_BASE El2=SK_IND_TOP Exit=SK_TMP
NOVA LOAD_IC_PUSH_WR Op=Write El1=SK_TMP El2=SK_IND_COUNT
NOVA LOAD_IC_DEEP_LV Op=Move El1=SK_IND_TOP Exit=SK_IND_LEVEL
NOVA LOAD_IC_DEEP_A Op=Move El1=C_2 Exit=SK_IND_ACTION
NOVA LOAD_IC_DEEP_J Op=Jump Exit=LOAD_IC_DONE
NOLINK
RVOCA LOAD_IC_SKIP Body=LOAD_IC_SET_SKIP
RREDI LOAD_IC_DONE

SET LOAD_IC_SET_SKIP Value=LOAD_IC_SS_IMPL
NOLINK
NOVA LOAD_IC_SS_IMPL Op=Move El1=C_0 Exit=SK_IND_ACTION
RREDI LOAD_IC_SS_RET

── LOAD_INDENT_STACK_INIT: start a fresh level-0 stack ──
//
Mirrors IndentStack.__init__(opener_indent): call once when a macro
header line is seen, before any LOAD_INDENT_CLASSIFY call for that
context's body. Sets SK_IND_TOP=0 and SK_IND_STACK_BASE[0]=opener_indent.
Wired now (see LOAD_READ_BODY, below) -- previously written but never
called, which let SK_IND_TOP leak stale state across separate FOR/SAVE
body reads; found and fixed together.

IN: SK_IND_COUNT must already hold the header line's own exact indent
    (i.e. call LOAD_READ_LINE on the header line itself first, or
    otherwise have SK_IND_COUNT set to opener_indent before calling).
    Not independently re-verified for every LOAD_READ_BODY caller --
    the reset itself (SK_IND_TOP=0) is unconditionally correct and is
    what fixes the confirmed bug; SK_IND_STACK_BASE[0]'s exact value
    matters only for callers whose body's first real line sits at a
    non-obvious indent relative to the header, a narrower claim than
    "this fix is complete" and worth someone rechecking against a real
    FOR/SAVE body with unusual header indentation.
//
SET LOAD_INDENT_STACK_INIT Value=LOAD_ISI_IMPL
NOLINK
CLEAR LOAD_ISI_IMPL El1=SK_IND_TOP
NOVA LOAD_ISI_WR Op=Write El1=SK_IND_STACK_BASE El2=SK_IND_COUNT
RREDI LOAD_ISI_RET

── LOAD_CTX_PUSH: push a new context onto the context stack ──
//
IN: SK_TMP3 = mode (CTX_MODE_*).
    SK_IND_COUNT = opener line's indent.
Saves enclosing context's indent state into the new slot, then
reinits SK_IND_TOP/SK_IND_STACK for the new context.
OUT: SK_CTX_SLOT_ADDR = address of the new slot.
//
SET LOAD_CTX_PUSH Value=LOAD_CP_IMPL2
NOLINK
/compute slot addr: base + SP * SLOT_SIZE
NOVA LOAD_CP_IMPL2 Op=Mul El1=SK_CTX_SP El2=CTX_SLOT_SIZE Exit=SK_CTX_TMP
NOVA LOAD_CP_ADDR2 Op=Add El1=SK_CTX_SLOTS_BASE El2=SK_CTX_TMP Exit=SK_CTX_SLOT_ADDR
/[0] = mode
NOVA LOAD_CP_WMODE2 Op=Write El1=SK_CTX_SLOT_ADDR El2=SK_TMP3
/[1] = opener_indent
NOVA LOAD_CP_OI_ADDR Op=Add El1=SK_CTX_SLOT_ADDR El2=C_1 Exit=SK_CTX_TMP
NOVA LOAD_CP_WOI Op=Write El1=SK_CTX_TMP El2=SK_IND_COUNT
/[2] = save current SK_IND_TOP (enclosing context's state)
NOVA LOAD_CP_IT_ADDR Op=Add El1=SK_CTX_SLOT_ADDR El2=C_2 Exit=SK_CTX_TMP
NOVA LOAD_CP_WIT Op=Write El1=SK_CTX_TMP El2=SK_IND_TOP
/[CTX_OFF_IND_LVL .. CTX_OFF_DATA_PTR-1] = save SK_IND_STACK via copy loop
RVOCA LOAD_CP_SAVE_IND Body=LOAD_CTX_SAVE_IND_STACK
/[CTX_OFF_DATA_PTR] = data_ptr (0 for now, caller fills in if needed)
NOVA LOAD_CP_DP_OFF Op=Move El1=CTX_OFF_DATA_PTR Exit=SK_CTX_TMP
NOVA LOAD_CP_DP_ADDR Op=Add El1=SK_CTX_SLOT_ADDR El2=SK_CTX_TMP Exit=SK_CTX_TMP
CLEAR LOAD_CP_WDP El1=SK_CTX_TMP
/increment SP
NOVA LOAD_CP_INCSP Op=Add El1=SK_CTX_SP El2=C_1 Exit=SK_CTX_SP
/reinit indent stack for the new context
RVOCA LOAD_CP_ISI Body=LOAD_INDENT_STACK_INIT
RREDI LOAD_CP_RET2

── LOAD_CTX_SAVE_IND_STACK: copy SK_IND_STACK into slot ind_levels ──
SET LOAD_CTX_SAVE_IND_STACK Value=LOAD_CSIS_IMPL
NOLINK
CLEAR LOAD_CSIS_IMPL El1=SK_CTX_TMP
/loop: SK_CTX_TMP = 0..SK_IND_STACK_SIZE-1
JEQ LOAD_CSIS_CK El1=SK_CTX_TMP El2=SK_IND_STACK_SIZE Exit=LOAD_CSIS_DONE
/src = SK_IND_STACK_BASE + i
NOVA LOAD_CSIS_SRC Op=Add El1=SK_IND_STACK_BASE El2=SK_CTX_TMP Exit=SK_TMP
NOVA LOAD_CSIS_RD Op=Read El1=SK_TMP Exit=SK_TMP2
/dst = SK_CTX_SLOT_ADDR + CTX_OFF_IND_LVL + i
NOVA LOAD_CSIS_OFF Op=Add El1=CTX_OFF_IND_LVL El2=SK_CTX_TMP Exit=SK_TMP
NOVA LOAD_CSIS_DST Op=Add El1=SK_CTX_SLOT_ADDR El2=SK_TMP Exit=SK_TMP
NOVA LOAD_CSIS_WR Op=Write El1=SK_TMP El2=SK_TMP2
NOVA LOAD_CSIS_INC Op=Add El1=SK_CTX_TMP El2=C_1 Exit=SK_CTX_TMP
NOVA LOAD_CSIS_JMP Op=Jump Exit=LOAD_CSIS_CK
NOLINK
RREDI LOAD_CSIS_DONE

── LOAD_CTX_POP: pop top context, restore enclosing indent state ──
//
Decrements SK_CTX_SP, restores SK_IND_TOP and SK_IND_STACK from the
popped slot's saved values. Caller should handle close logic BEFORE
calling pop (read the slot's mode/data_ptr to finalize).
OUT: SK_CTX_SLOT_ADDR = address of the POPPED slot (still readable).
//
SET LOAD_CTX_POP Value=LOAD_CPOP_IMPL
NOLINK
/decrement SP
NOVA LOAD_CPOP_IMPL Op=Sub El1=SK_CTX_SP El2=C_1 Exit=SK_CTX_SP
/compute popped slot addr: base + SP * SLOT_SIZE
NOVA LOAD_CPOP_MUL Op=Mul El1=SK_CTX_SP El2=CTX_SLOT_SIZE Exit=SK_CTX_TMP
NOVA LOAD_CPOP_ADDR Op=Add El1=SK_CTX_SLOTS_BASE El2=SK_CTX_TMP Exit=SK_CTX_SLOT_ADDR
/restore SK_IND_TOP from slot[2]
NOVA LOAD_CPOP_IT_ADDR Op=Add El1=SK_CTX_SLOT_ADDR El2=C_2 Exit=SK_CTX_TMP
NOVA LOAD_CPOP_RIT Op=Read El1=SK_CTX_TMP Exit=SK_IND_TOP
/restore SK_IND_STACK from slot ind_levels
RVOCA LOAD_CPOP_RIND Body=LOAD_CTX_RESTORE_IND_STACK
RREDI LOAD_CPOP_RET

── LOAD_CTX_RESTORE_IND_STACK: copy slot ind_levels back to SK_IND_STACK ──
SET LOAD_CTX_RESTORE_IND_STACK Value=LOAD_CRIS_IMPL
NOLINK
CLEAR LOAD_CRIS_IMPL El1=SK_CTX_TMP
JEQ LOAD_CRIS_CK El1=SK_CTX_TMP El2=SK_IND_STACK_SIZE Exit=LOAD_CRIS_DONE
/src = SK_CTX_SLOT_ADDR + CTX_OFF_IND_LVL + i
NOVA LOAD_CRIS_OFF Op=Add El1=CTX_OFF_IND_LVL El2=SK_CTX_TMP Exit=SK_TMP
NOVA LOAD_CRIS_SRC Op=Add El1=SK_CTX_SLOT_ADDR El2=SK_TMP Exit=SK_TMP
NOVA LOAD_CRIS_RD Op=Read El1=SK_TMP Exit=SK_TMP2
/dst = SK_IND_STACK_BASE + i
NOVA LOAD_CRIS_DST Op=Add El1=SK_IND_STACK_BASE El2=SK_CTX_TMP Exit=SK_TMP
NOVA LOAD_CRIS_WR Op=Write El1=SK_TMP El2=SK_TMP2
NOVA LOAD_CRIS_INC Op=Add El1=SK_CTX_TMP El2=C_1 Exit=SK_CTX_TMP
NOVA LOAD_CRIS_JMP Op=Jump Exit=LOAD_CRIS_CK
NOLINK
RREDI LOAD_CRIS_DONE

── LOAD_CTX_PEEK: compute address of current top slot ──
//
OUT: SK_CTX_SLOT_ADDR = base + (SP-1) * SLOT_SIZE.
     Caller must check SK_CTX_SP != 0 first.
//
SET LOAD_CTX_PEEK Value=LOAD_CPEK_IMPL
NOLINK
NOVA LOAD_CPEK_IMPL Op=Sub El1=SK_CTX_SP El2=C_1 Exit=SK_CTX_TMP
NOVA LOAD_CPEK_MUL Op=Mul El1=SK_CTX_TMP El2=CTX_SLOT_SIZE Exit=SK_CTX_TMP
NOVA LOAD_CPEK_ADDR Op=Add El1=SK_CTX_SLOTS_BASE El2=SK_CTX_TMP Exit=SK_CTX_SLOT_ADDR
RREDI LOAD_CPEK_RET

── LOAD_CTX_CLOSE_TOP: mode-specific close for the current top context ──
//
Called by the dedent-close loop before LOAD_CTX_POP. Reads the top
slot's mode and dispatches to the appropriate close handler.
IN: SK_CTX_SLOT_ADDR already set by LOAD_CTX_PEEK.
//
SET LOAD_CTX_CLOSE_TOP Value=LOAD_CCT_IMPL
NOLINK
NOVA LOAD_CCT_IMPL Op=Read El1=SK_CTX_SLOT_ADDR Exit=SK_TMP
JEQ LOAD_CCT_MSAVE El1=SK_TMP El2=CTX_MODE_SAVE Exit=LOAD_CCT_SAVE
RREDI LOAD_CCT_STUB
NOLINK
/── SAVE close: restore persisted RA_MA + SK_SES_SREG, emit restore NOVAs ──
NOVA LOAD_CCT_SAVE Op=Add El1=SK_CTX_SLOT_ADDR El2=CTX_OFF_DATA_PTR Exit=SK_TMP
NOVA LOAD_CCT_RDP Op=Read El1=SK_TMP Exit=SK_TMP
/restore RA_MA1..4 from data[0..3]
NOVA LOAD_CCT_RM1 Op=Read El1=SK_TMP Exit=RA_MA1
NOVA LOAD_CCT_A1 Op=Add El1=SK_TMP El2=C_1 Exit=SK_TMP2
NOVA LOAD_CCT_RM2 Op=Read El1=SK_TMP2 Exit=RA_MA2
NOVA LOAD_CCT_A2 Op=Add El1=SK_TMP El2=C_2 Exit=SK_TMP2
NOVA LOAD_CCT_RM3 Op=Read El1=SK_TMP2 Exit=RA_MA3
NOVA LOAD_CCT_A3 Op=Add El1=SK_TMP El2=C_3 Exit=SK_TMP2
NOVA LOAD_CCT_RM4 Op=Read El1=SK_TMP2 Exit=RA_MA4
/restore SK_SES_SREG_1..4 from data[4..7]
NOVA LOAD_CCT_A4 Op=Add El1=SK_TMP El2=C_4 Exit=SK_TMP2
NOVA LOAD_CCT_RS1 Op=Read El1=SK_TMP2 Exit=SK_TMP3
NOVA LOAD_CCT_WS1 Op=Write El1=SK_SES_SREG_1 El2=SK_TMP3
NOVA LOAD_CCT_A5 Op=Add El1=SK_TMP El2=C_5 Exit=SK_TMP2
NOVA LOAD_CCT_RS2 Op=Read El1=SK_TMP2 Exit=SK_TMP3
NOVA LOAD_CCT_WS2 Op=Write El1=SK_SES_SREG_2 El2=SK_TMP3
NOVA LOAD_CCT_A6 Op=Add El1=SK_TMP El2=C_6 Exit=SK_TMP2
NOVA LOAD_CCT_RS3 Op=Read El1=SK_TMP2 Exit=SK_TMP3
NOVA LOAD_CCT_WS3 Op=Write El1=SK_SES_SREG_3 El2=SK_TMP3
NOVA LOAD_CCT_A7 Op=Add El1=SK_TMP El2=C_7 Exit=SK_TMP2
NOVA LOAD_CCT_RS4 Op=Read El1=SK_TMP2 Exit=SK_TMP3
NOVA LOAD_CCT_WS4 Op=Write El1=SK_SES_SREG_4 El2=SK_TMP3
RVOCA LOAD_CCT_REST Body=SAVE_EMIT_RESTORES
RREDI LOAD_CCT_SRET

── LOAD_READ_TOKEN, LOAD_DECLARE_TOKEN, LOAD_RESOLVE_TOKEN: protocol stubs ──
//
Three-verb protocol so macro bodies never call wax.re's low-level
BS_READ_TOKEN/BS_INTERN/LOAD_INTERN directly, each by name, scattered
throughout -- one seam instead of many.

READ    — get the next raw token, no interpretation.
DECLARE — the current token is a NEW name being introduced right now.
          Self-ref on "not found" is correct here (that's what
          declaring a brand-new name means: there's nothing to find
          yet, and this call is what creates it).
RESOLVE — the current token is a REFERENCE to something else, which
          may not be declared yet (LOAD_MAIN's interleaved file
          scheduling). Caller MUST set up backfill support before
          calling.

DECLARE_TOKEN is currently the same underlying call as the old,
unsafe path -- NOT yet switched to the safe LOAD_INTERN. BS_INTERN
creates a hashtable entry even on a miss (a self-ref stub); LOAD_INTERN
creates a real, backfillable entry instead. Some existing caller may
silently depend on "this name has SOME entry, even if wrong" -- the
plan is one caller at a time, verifying self-hosting stays 42/42 after
each switch, not a blanket change here.

(Comment reconstructed from a garbled, interleaved version of itself
found during this session's duplicate-hunting pass -- the underlying
lines had been shuffled somehow, alternating stray "//" with fragments
that didn't read in sequence. Best-effort reassembly by fragment
content, not a verified-verbatim recovery of the original wording; the
code itself was untouched and is not in question.)
//
SET LOAD_READ_TOKEN Value=LOAD_RT_IMPL
NOLINK
RVOCA LOAD_RT_IMPL Body=BS_READ_TOKEN
RREDI LOAD_RT_RRET

SET LOAD_DECLARE_TOKEN Value=LOAD_DT_IMPL
NOLINK
RVOCA LOAD_DT_IMPL Body=BS_INTERN
NOVA LOAD_DT_RES Op=Move El1=RA_BS_RESULT Exit=RA_LOAD_RESULT
RREDI LOAD_DT_RRET

SET LOAD_RESOLVE_TOKEN Value=LOAD_RS_IMPL
NOLINK
RVOCA LOAD_RS_IMPL Body=LOAD_INTERN
NOVA LOAD_RS_RES Op=Move El1=RA_BS_RESULT Exit=RA_LOAD_RESULT
RREDI LOAD_RS_RRET

── LOAD_READ_BODY: read indented lines into body buffer ───────
//
Each line is prefixed with its indent depth byte (1 or 2) as a marker.
Stops at non-indented line or EOF (line NOT consumed).
//
── LOAD_INDENT_MARK: classify, publish level, derive body marker ──
//
Bridges the exact indent stack to the two-valued marker its consumers
actually want.

Runs LOAD_INDENT_CLASSIFY on SK_IND_COUNT (already measured by
LOAD_COUNT_INDENT at line start), then:
  RA_LOAD_INDENT = the exact level (0 = no indent / inert, else 1,2,3...)
  SK_IND_DEPTH   = 0 if no indent, 1 if level 1, 2 if level >= 2

RA_LOAD_INDENT is the register registers.re already declared for this
("current indentation level (0=none, 1=one, ...)") and which nothing
used until now -- its open-ended semantics match the real level, so the
precise value stays available to anything that wants it.

SK_IND_DEPTH stays deliberately two-valued. LOAD_EXPAND_TEMPLATE
distinguishes exactly two cases -- C_1 = body line, C_2 = override line
-- so handing it uncapped levels would leave a level-3 line matching
neither marker, and its bytes would leak into emitted content. Two
states are all the expander needs, so capping here is the honest
representation, not a workaround. Depth beyond 2 is still carried
faithfully by RA_LOAD_INDENT for callers that care.
//
SET LOAD_INDENT_MARK Value=LOAD_IM_IMPL
NOLINK
RVOCA LOAD_IM_IMPL Body=LOAD_INDENT_CLASSIFY
CLEAR LOAD_IM_Z0 El1=RA_LOAD_INDENT
CLEAR LOAD_IM_Z1 El1=SK_IND_DEPTH
JZ LOAD_IM_ACTCK El1=SK_IND_ACTION Exit=LOAD_IM_DONE
/action != skip: SK_IND_LEVEL is meaningful (>= 1)
NOVA LOAD_IM_LVL Op=Move El1=SK_IND_LEVEL Exit=RA_LOAD_INDENT
NOVA LOAD_IM_MK2 Op=Move El1=C_2 Exit=SK_IND_DEPTH
JEQ LOAD_IM_L1CK El1=SK_IND_LEVEL El2=C_1 Exit=LOAD_IM_MK1
RREDI LOAD_IM_DONE
NOLINK
NOVA LOAD_IM_MK1 Op=Move El1=C_1 Exit=SK_IND_DEPTH
NOVA LOAD_IM_MK1J Op=Jump Exit=LOAD_IM_DONE

SET LOAD_READ_BODY Value=LOAD_RB_IMPL
NOLINK
/Reset the indent stack before this body's own first line -- LOAD_
/READ_BODY is called more than once per parse (once per FOR/SAVE
/occurrence), and LOAD_INDENT_CLASSIFY's own doc says SK_IND_TOP is
/only touched on 'deeper'/'return', never 'skip' -- so a body ending
/on a 'skip'-classified line (the common case: landing back on the
/opener's own indent) leaves SK_IND_TOP at whatever nonzero depth the
/body's own nesting last reached, not reset to 0. A second body read
/right after would then classify against a stale, wrong stack from the
/first one. LOAD_INDENT_STACK_INIT existed for exactly this and was
/never wired in; found and fixed together, not two separate things.
NOVA LOAD_RB_IMPL Op=Move El1=SK_BODY_BASE Exit=SK_BODY_PTR
RVOCA LOAD_RB_ISI Body=LOAD_INDENT_STACK_INIT
CLEAR LOAD_RB_LC El1=SK_BODY_LINES
RVOCA LOAD_RB_RL Body=LOAD_READ_LINE
JZ LOAD_RB_EOFCK El1=RA_LOAD_BYTE Exit=LOAD_RB_DONE
/LOAD_READ_LINE leaves the exact indent in SK_IND_COUNT; LOAD_INDENT_MARK
/classifies it against the level stack and derives SK_IND_DEPTH (0 = not
/indented, so the body ends here; 1 = body line; 2 = override line).
RVOCA LOAD_RB_MARKC Body=LOAD_INDENT_MARK
JZ LOAD_RB_INDCK El1=SK_IND_DEPTH Exit=LOAD_RB_DONE
/Buffer layout per line: [depth-marker byte][content bytes...][LF].
/LOAD_PROCESS_BODY / LOAD_EXPAND_TEMPLATE both split lines on that LF.
/Overflow (no room even for the marker): stop buffering the whole body.
JEQ LOAD_RB_OVFLCK El1=SK_BODY_PTR El2=SK_BODY_LIMIT Exit=LOAD_RB_DONE
NOVA LOAD_RB_MARK Op=Write El1=SK_BODY_PTR El2=SK_IND_DEPTH
NOVA LOAD_RB_MINC Op=Add El1=SK_BODY_PTR El2=C_1 Exit=SK_BODY_PTR
NOVA LOAD_RB_STORE Op=Write El1=SK_BODY_PTR El2=RA_LOAD_BYTE
NOVA LOAD_RB_INC Op=Add El1=SK_BODY_PTR El2=C_1 Exit=SK_BODY_PTR
/Copy the REST of this line until LF/EOF. LOAD_READ_LINE consumed the
/indent and returned only the FIRST content byte (stored just above); the
/remaining bytes are still in the stream. Storing just that first byte --
/the old behavior -- left every buffered line one byte long, and its tail
/leaked into the next LOAD_READ_LINE, which misread it as a dedent and
/ended the body after a single character (never triggered before: no real
/SAVE/FOR self-hosted body had run). See BUGS.md Pattern Z.
RVOCA LOAD_RB_CH Body=BS_READ_BYTE
JZ LOAD_RB_CHEOF El1=RA_LOAD_BYTE Exit=LOAD_RB_EOL
JEQ LOAD_RB_CHLF El1=RA_LOAD_BYTE El2=LF Exit=LOAD_RB_EOL
/Buffer full mid-line: stop copying this line's tail, still terminate it.
JEQ LOAD_RB_CHOVF El1=SK_BODY_PTR El2=SK_BODY_LIMIT Exit=LOAD_RB_EOL
NOVA LOAD_RB_CHW Op=Write El1=SK_BODY_PTR El2=RA_LOAD_BYTE
NOVA LOAD_RB_CHINC Op=Add El1=SK_BODY_PTR El2=C_1 Exit=SK_BODY_PTR
NOVA LOAD_RB_CHJ Op=Jump Exit=LOAD_RB_CH
NOLINK
/Terminate the buffered line with LF, count it. If the tail-read hit EOF
/(RA_LOAD_BYTE == 0), this was the final line -- terminate then stop.
NOVA LOAD_RB_EOL Op=Write El1=SK_BODY_PTR El2=LF
NOVA LOAD_RB_EOLINC Op=Add El1=SK_BODY_PTR El2=C_1 Exit=SK_BODY_PTR
NOVA LOAD_RB_LINC Op=Add El1=SK_BODY_LINES El2=C_1 Exit=SK_BODY_LINES
JZ LOAD_RB_EOLEOF El1=RA_LOAD_BYTE Exit=LOAD_RB_DONE
NOVA LOAD_RB_JMP Op=Jump Exit=LOAD_RB_RL
NOLINK
RREDI LOAD_RB_DONE

── LOAD_EXPAND_TEMPLATE: shared body-dispatcher (FOR + SAVE) ──
//
Walks the SK_BODY buffer LOAD_READ_BODY filled, splits it into lines on
the per-line LF, and dispatches each via LOAD_DISPATCH_BUILT_LINE. For a
FOR element it also substitutes {X} → element name and {N} → decimal
index of RA_FOR_IDX; a SAVE body has no placeholders, so those paths are
inert and it behaves as a plain per-line dispatcher (this is why SAVE
calls it instead of a separate routine -- the old LOAD_PROCESS_BODY was
exactly this minus substitution, now merged away).
//
SET LOAD_EXPAND_TEMPLATE Value=LOAD_ET_IMPL
NOLINK
NOVA LOAD_ET_IMPL Op=Move El1=SK_BODY_BASE Exit=SK_ET_PTR
NOVA LOAD_ET_LINE Op=Move El1=BS_TOKBUF_BASE Exit=SK_ET_OUT
CLEAR LOAD_ET_DINIT El1=SK_ET_DONE
JEQ LOAD_ET_LOOP El1=SK_ET_PTR El2=SK_BODY_PTR Exit=LOAD_ET_ENDCK
NOVA LOAD_ET_RBYTE Op=Read El1=SK_ET_PTR Exit=SK_TMP
NOVA LOAD_ET_INC Op=Add El1=SK_ET_PTR El2=C_1 Exit=SK_ET_PTR
/Skip indent marker bytes. C_1 = body line, process normally. C_2 =
/override line -- these are consulted for values by LOAD_CF_RESOLVE_
/OVERRIDE, not dispatched as their own commands; skip the whole line's
/content here (previously both were treated identically -- a real bug,
/since an override line like "SK_WB_H_ALIAS  > Y=ALIAS" would otherwise
/get expanded and dispatched as if it were body content). See BUGS.md
/Pattern Z.
JEQ LOAD_ET_MRK1 El1=SK_TMP El2=C_1 Exit=LOAD_ET_LOOP
JEQ LOAD_ET_MRK2 El1=SK_TMP El2=C_2 Exit=LOAD_ET_SKIPOV
/LF → flush line
JEQ LOAD_ET_LFCK El1=SK_TMP El2=LF Exit=LOAD_ET_FLUSH_DISPATCH
/'{' → placeholder
JEQ LOAD_ET_LBCK El1=SK_TMP El2=LBRACE Exit=LOAD_ET_PLACEHOLDER
/Ordinary byte
WRITE_OUT LOAD_ET_WBYTE SK_TMP
NOVA LOAD_ET_JMP Op=Jump Exit=LOAD_ET_LOOP
NOLINK
JEQ LOAD_ET_SKIPOV El1=SK_ET_PTR El2=SK_BODY_PTR Exit=LOAD_ET_ENDCK
NOVA LOAD_ET_SOVR Op=Read El1=SK_ET_PTR Exit=SK_TMP
NOVA LOAD_ET_SOVI Op=Add El1=SK_ET_PTR El2=C_1 Exit=SK_ET_PTR
JEQ LOAD_ET_SOVLF El1=SK_TMP El2=LF Exit=LOAD_ET_LOOP
NOVA LOAD_ET_SOVJ Op=Jump Exit=LOAD_ET_SKIPOV
NOLINK
CLEAR LOAD_ET_PLACEHOLDER El1=SK_ET_PH_IDX
NOVA LOAD_ET_PH_RD Op=Read El1=SK_ET_PTR Exit=SK_TMP
NOVA LOAD_ET_PH_INC Op=Add El1=SK_ET_PTR El2=C_1 Exit=SK_ET_PTR
JEQ LOAD_ET_PH_RBCK El1=SK_TMP El2=RBRACE Exit=LOAD_ET_PH_DISPATCH
NOVA LOAD_ET_PH_IINC Op=Add El1=SK_ET_PH_IDX El2=C_1 Exit=SK_ET_PH_IDX
NOVA LOAD_ET_PH_JMP Op=Jump Exit=LOAD_ET_PH_RD
NOLINK
NOVA LOAD_ET_PH_DISPATCH Op=Move El1=BS_TOKBUF_BASE Exit=SK_FLAG
NOVA LOAD_ET_PH_RB0 Op=Read El1=SK_FLAG Exit=SK_TMP
JEQ LOAD_ET_PH_XCK El1=SK_TMP El2=ASCII_X Exit=LOAD_ET_EMIT_ELEM
JEQ LOAD_ET_PH_NCK El1=SK_TMP El2=ASCII_N Exit=LOAD_ET_EMIT_IDX
/Unknown placeholder: passthrough '{' + content
NOVA LOAD_ET_PH_UNK Op=Move El1=LBRACE Exit=SK_TMP
WRITE_OUT LOAD_ET_PH_UW SK_TMP
NOVA LOAD_ET_PH_UJMP Op=Jump Exit=LOAD_ET_LOOP
NOLINK
/Emit element name bytes from FOR_IRIS buffers
NOVA LOAD_ET_EMIT_ELEM Op=Move El1=RA_FOR_IDX Exit=SK_TMP
NOVA LOAD_ET_EE_IDXA Op=Add El1=RA_FOR_IRIS_IDX_BASE_ADDR El2=SK_TMP Exit=SK_TMP2
NOVA LOAD_ET_EE_OFFSET Op=Read El1=SK_TMP2 Exit=SK_TMP
NOVA LOAD_ET_EE_NBASE Op=Add El1=RA_FOR_IRIS_BASE_ADDR El2=SK_TMP Exit=SK_TMP2
NOVA LOAD_ET_EE_LOOP Op=Read El1=SK_TMP2 Exit=SK_TMP
JZ LOAD_ET_EE_NULCK El1=SK_TMP Exit=LOAD_ET_EE_DONE
WRITE_OUT LOAD_ET_EE_WRITE SK_TMP
NOVA LOAD_ET_EE_NI Op=Add El1=SK_TMP2 El2=C_1 Exit=SK_TMP2
NOVA LOAD_ET_EE_JMP2 Op=Jump Exit=LOAD_ET_EE_LOOP
NOLINK
NOVA LOAD_ET_EE_DONE Op=Jump Exit=LOAD_ET_LOOP
NOLINK
/Emit index as decimal
NOVA LOAD_ET_EMIT_IDX Op=Move El1=RA_FOR_IDX Exit=SK_TMP
RVOCA LOAD_ET_EI_INT Body=LOAD_EMIT_INT_BYTES
NOVA LOAD_ET_EI_JMP Op=Jump Exit=LOAD_ET_LOOP
//
End of body reached. Flush whatever's pending exactly once — if we land
here again (SK_ET_DONE already 1), the body was already fully flushed and
nothing changed since (SK_ET_PTR/SK_BODY_PTR never advance past this
point), so looping into FLUSH_DISPATCH again would just re-dispatch an
empty line forever.
//
NOLINK
JZ LOAD_ET_ENDCK El1=SK_ET_DONE Exit=LOAD_ET_EDOFL
RREDI LOAD_ET_DRET
NOLINK
NOVA LOAD_ET_EDOFL Op=Move El1=C_1 Exit=SK_ET_DONE
NOVA LOAD_ET_EDJMP Op=Jump Exit=LOAD_ET_FLUSH_DISPATCH
NOLINK
NOVA LOAD_ET_FLUSH_DISPATCH Op=Write El1=SK_ET_OUT El2=LF
NOVA LOAD_ET_FD_INC Op=Add El1=SK_ET_OUT El2=C_1 Exit=SK_ET_OUT
NOVA LOAD_ET_FD_FB Op=Read El1=BS_TOKBUF_BASE Exit=RA_LOAD_BYTE
NOVA LOAD_ET_FD_LEN Op=Sub El1=SK_ET_OUT El2=BS_TOKBUF_BASE Exit=RA_LOAD_TLEN
/Content is already in BS_TOKBUF_BASE (written via SK_ET_OUT above) --
/LOAD_DISPATCH_CORE reads BS_TOKBUF_BASE/RA_LOAD_TLEN directly (its own
/documented contract), so no copy to any other buffer is needed. The
/previous version copied to BS_READBUF_BASE (a different mechanism
/entirely, for BS_READ_BYTE's own normal file-stream reads) and called
/LOAD_DISPATCH_LINE, which re-reads a *fresh* token from the file
/stream, discarding the just-built expansion entirely -- the exact
/mistake LOAD_DISPATCH_CORE's own comment warns against. See BUGS.md
/Pattern Z.
RVOCA LOAD_ET_FD_CPDONE Body=LOAD_DISPATCH_BUILT_LINE
NOVA LOAD_ET_FD_RESET Op=Move El1=BS_TOKBUF_BASE Exit=SK_ET_OUT
NOVA LOAD_ET_FD_JMP Op=Jump Exit=LOAD_ET_LOOP
NOLINK

── LOAD_EMIT_INT_BYTES: write decimal digits of SK_TMP → SK_ET_OUT ──
/Handles 0-9999.
SET LOAD_EMIT_INT_BYTES Value=LOAD_EIB_IMPL
NOLINK
NOVA LOAD_EIB_IMPL Op=Move El1=SK_TMP Exit=SK_EIB_VAL
NOVA LOAD_EIB_D100 Op=Div El1=SK_EIB_VAL El2=C_100 Exit=SK_EIB_DIV
NOVA LOAD_EIB_R100 Op=Rem El1=SK_EIB_VAL El2=C_100 Exit=SK_EIB_VAL
JZ LOAD_EIB_CK100 El1=SK_EIB_DIV Exit=LOAD_EIB_TENS
NOVA LOAD_EIB_A100 Op=Add El1=SK_EIB_DIV El2=ASCII_0 Exit=SK_TMP
WRITE_OUT LOAD_EIB_W100 SK_TMP
NOVA LOAD_EIB_TENS Op=Div El1=SK_EIB_VAL El2=C_10 Exit=SK_EIB_DIV
NOVA LOAD_EIB_RTENS Op=Rem El1=SK_EIB_VAL El2=C_10 Exit=SK_EIB_VAL
JZ LOAD_EIB_CKTENS El1=SK_EIB_DIV Exit=LOAD_EIB_ONES
NOVA LOAD_EIB_ATENS Op=Add El1=SK_EIB_DIV El2=ASCII_0 Exit=SK_TMP
WRITE_OUT LOAD_EIB_WTENS SK_TMP
NOVA LOAD_EIB_ONES Op=Add El1=SK_EIB_VAL El2=ASCII_0 Exit=SK_TMP
WRITE_OUT LOAD_EIB_WONES SK_TMP
RREDI LOAD_EIB_RRET

── LOAD_PROCESS_BODY: removed -- merged into LOAD_EXPAND_TEMPLATE ─────
/It was LOAD_EXPAND_TEMPLATE minus the {X}/{N} placeholder substitution:
/both walk the SK_BODY buffer, skip the depth/override markers, split
/lines on LF, and dispatch each via LOAD_DISPATCH_BUILT_LINE. SAVE bodies
/carry no placeholders, so LOAD_EXPAND_TEMPLATE reproduces LOAD_PROCESS_
/BODY's behavior exactly for them; SAVE now calls it directly (see
/EXP_SAVE_START) and this near-duplicate (and its SK_PB_PTR / LD_NI_OUT
/scratch) is gone. One body-dispatcher, shared by SAVE and FOR.

── SAVE_EMIT_SAVES: alloc Move reg→S_reg NOVA luces ───────────
SET SAVE_EMIT_SAVES Value=SAVE_ES_IMPL
NOLINK
JZ SAVE_ES_IMPL El1=RA_MA1 Exit=SAVE_ES_DONE
NOVA SAVE_ES_D1 Op=Move El1=SK_SES_SREG_1 Exit=SK_SES_SDST
RVOCA SAVE_ES_DO1 Body=SAVE_EMIT_ONE_SAVE
JZ SAVE_ES_CK2 El1=RA_MA2 Exit=SAVE_ES_DONE
NOVA SAVE_ES_MV2 Op=Move El1=RA_MA2 Exit=RA_MA1
NOVA SAVE_ES_D2 Op=Move El1=SK_SES_SREG_2 Exit=SK_SES_SDST
RVOCA SAVE_ES_DO2 Body=SAVE_EMIT_ONE_SAVE
JZ SAVE_ES_CK3 El1=RA_MA3 Exit=SAVE_ES_DONE
NOVA SAVE_ES_MV3 Op=Move El1=RA_MA3 Exit=RA_MA1
NOVA SAVE_ES_D3 Op=Move El1=SK_SES_SREG_3 Exit=SK_SES_SDST
RVOCA SAVE_ES_DO3 Body=SAVE_EMIT_ONE_SAVE
JZ SAVE_ES_CK4 El1=RA_MA4 Exit=SAVE_ES_DONE
NOVA SAVE_ES_MV4 Op=Move El1=RA_MA4 Exit=RA_MA1
NOVA SAVE_ES_D4 Op=Move El1=SK_SES_SREG_4 Exit=SK_SES_SDST
RVOCA SAVE_ES_DO4 Body=SAVE_EMIT_ONE_SAVE
RREDI SAVE_ES_DONE

── SAVE_EMIT_ONE_SAVE: alloc S_reg lux + Move NOVA ───────────
SET SAVE_EMIT_ONE_SAVE Value=SAVE_EOS_IMPL
NOLINK
ALLOC_TO SAVE_EOS_IMPL Exit=SK_SES_SREG Count=C_1
NOVA SAVE_EOS_SDST Op=Write El1=SK_SES_SDST El2=SK_SES_SREG
NOVA SAVE_EOS_SRC Op=Move El1=RA_MA1 Exit=SK_SES_SRC
NOVA SAVE_EOS_DST Op=Move El1=SK_SES_SREG Exit=SK_SES_DST
RVOCA SAVE_EOS_EML Body=SAVE_EMIT_MOVE_LUX
RREDI SAVE_EOS_RRET

── SAVE_EMIT_RESTORES: emit Move S_reg→reg luces ─────────────
SET SAVE_EMIT_RESTORES Value=SAVE_ER_IMPL
NOLINK
JZ SAVE_ER_IMPL El1=RA_MA1 Exit=SAVE_ER_DONE
NOVA SAVE_ER_SD1 Op=Read El1=SK_SES_SREG_1 Exit=SK_SES_SREG
RVOCA SAVE_ER_DO1 Body=SAVE_EMIT_ONE_RESTORE
JZ SAVE_ER_CK2 El1=RA_MA2 Exit=SAVE_ER_DONE
NOVA SAVE_ER_MV2 Op=Move El1=RA_MA2 Exit=RA_MA1
NOVA SAVE_ER_SD2 Op=Read El1=SK_SES_SREG_2 Exit=SK_SES_SREG
RVOCA SAVE_ER_DO2 Body=SAVE_EMIT_ONE_RESTORE
JZ SAVE_ER_CK3 El1=RA_MA3 Exit=SAVE_ER_DONE
NOVA SAVE_ER_MV3 Op=Move El1=RA_MA3 Exit=RA_MA1
NOVA SAVE_ER_SD3 Op=Read El1=SK_SES_SREG_3 Exit=SK_SES_SREG
RVOCA SAVE_ER_DO3 Body=SAVE_EMIT_ONE_RESTORE
RREDI SAVE_ER_DONE

── SAVE_EMIT_ONE_RESTORE: alloc Move S_reg→reg NOVA ───────────
SET SAVE_EMIT_ONE_RESTORE Value=SAVE_EOR_IMPL
NOLINK
NOVA SAVE_EOR_IMPL Op=Move El1=SK_SES_SREG Exit=SK_SES_SRC
NOVA SAVE_EOR_DST Op=Move El1=RA_MA1 Exit=SK_SES_DST
RVOCA SAVE_EOR_EML Body=SAVE_EMIT_MOVE_LUX
RREDI SAVE_EOR_RRET

── SAVE_EMIT_MOVE_LUX: alloc NOVA_SIZE lux wired Move SK_SES_SRC→SK_SES_DST, link into chain ──
//
IN: SK_SES_SRC, SK_SES_DST = El1/Exit of the Move this lux becomes.
OUT: RA_ALLOC_RESULT = the new lux address.
Shared tail of SAVE_EMIT_ONE_SAVE/SAVE_EMIT_ONE_RESTORE -- the two were
identical from here on, differing only in which of {register, stack
slot} plays source vs dest, which is now the caller's own choice
(SK_SES_SRC/SK_SES_DST) instead of two copies of this same alloc+
self-ref+wire+link sequence.
//
NOLINK
NOVA SAVE_EML_ICN Op=Move El1=NOVA_SIZE Exit=RA_ALLOC_COUNT
RVOCA SAVE_EML_IAC Body=ALLOC
NOVA SAVE_EML_ISELF Op=Write El1=RA_ALLOC_RESULT El2=RA_ALLOC_RESULT
FOR C_1 C_2 C_4
    NOVA SAVE_EML_I{N} Op=Add El1=RA_ALLOC_RESULT El2={X} Exit=SK_TMP
    NOVA SAVE_EML_I{N}W Op=Write El1=SK_TMP El2={Y}
        C_1 > Y=Move
        C_2 > Y=SK_SES_SRC
        > Y=SK_SES_DST

NOVA SAVE_EML_LP Op=Move El1=RA_MC_PREV Exit=RA_NL_PREV
NOVA SAVE_EML_LN Op=Move El1=RA_ALLOC_RESULT Exit=RA_NL_NEXT
RVOCA SAVE_EML_LNK Body=LINK_NEXT
NOVA SAVE_EML_NOAUTO Op=Move El1=RA_ALLOC_RESULT Exit=RA_MC_PREV
RREDI SAVE_EML_RRET
NOLINK
/SAVE_EMIT_MOVE_LUX is the callable entry of this body (callers do RVOCA
/Body=SAVE_EMIT_MOVE_LUX, reading a[SAVE_EMIT_MOVE_LUX] as target). Without
/this alias it stayed 0 -- undefined call target (GL_IMPL class) -- and
/would fault when SAVE emitted during the load wave. Entry-alias idiom.
SET SAVE_EMIT_MOVE_LUX Value=SAVE_EML_ICN
