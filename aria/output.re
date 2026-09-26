//
aria/output.re — Output buffer, digit scratch, and output subroutines

── ARCHITECTURE ──────────────────────────────────────────────
One pipeline, one sink, one set of subroutines:
PUT_BYTE / EMIT_STR_ENTRY / EMIT_INT_ENTRY  — write into OB
PB_FLUSH                                    — drain OB to fd RA_OB_FD
FLUSH                                       — guarded PB_FLUSH

The destination file descriptor is parametric: RA_OB_FD.word is the fd
PB_FLUSH writes to. Default = STDOUT (1). Other code (wane.re) sets
it to STDERR (2) for the duration of its writes, FLUSHes between
boundaries, and restores it. The EMIT/PUT/FLUSH machinery does not
duplicate per destination — there is only one of each.

── OB (Output Buffer) ────────────────────────────────────────
OB_START: OB_SIZE consecutive Lux used as a byte buffer.
OB_START.word = OB_START's Lux ID (set via SEAL).
RA_OB_POS tracks how many bytes are currently buffered.
When full (RA_OB_POS == OB_SIZE), PB_FLUSH writes to RA_OB_FD and resets.

── DS (Digit Scratch) ────────────────────────────────────────
DS_START: 20 consecutive Lux used by EMIT_INT_ENTRY for digit reversal.
DS_START.word = DS_START's Lux ID.

── WHY SEAL FOR BASES ────────────────────────────────────────
"SPAN" creates a Lux whose word = 0 by default. Using the span name
directly as El1 would read word=0 → address 0 (NULL sentinel).
The SEAL'd register holds the actual Lux ID of the span's start.

── REGISTER DISCIPLINE ──────────────────────────────────────
RA_LINK is saved/restored automatically by the call stack (Voca/Redi).
Both are private to output.re — callers must not use them directly.
Callers may freely use RA_RET2 for their own nested-call needs.

SYSCALL CONVENTION (used in PB_FLUSH):
"Set" els via: Move El1=VALUE Exit=SC_Ax
SC_* are self-referential (SEAL SC_X SC_X), so Python reads
mem[SC_X_ID] = SC_X.word = the value set by Move.

DEPENDENCY: aspects.re, core/constants.re, aria/ascii.re, runtime/registers.re,
aria/io.re (STDOUT default sink),
runtime/registers.re, target/linux_generic.re

Single source of truth for the OB byte capacity.
OB buffer uses packed u64 luces: 8 bytes per lux.
OB_SIZE = byte capacity (256). OB_LUCES = lux count (32).
RA_OB_POS counts bytes (0..255). Byte i lives in:
  lux:  OB_START + (RA_OB_POS right-shift 3)
  shift: (RA_OB_POS AND 7) left-shift 3
OB_SIZE stays 256 so PB_FULL and SYS_WRITE SC_A2 remain byte-counts.
Interpreter reads packed luces via SYS_WRITE_PACKED syscall 103.
//
SET OB_SIZE Value=256
SET OB_LUCES Value=32

NEW OB_START  Count=OB_LUCES
NEW DS_START  Count=20

NEW OB_START In=OWN
NEW RA_OB_POS
NEW RA_OB_SHIFT  /persistent bit-shift counter within lux: 0,8,16,24,32,40,48,56 then reset
NEW RA_OB_ADDR  /persistent lux address = OB_BASE + (OB_POS >> 3); updated on new-lux and flush
NEW DS_START In=OWN
NEW RA_DS_POS
SET RA_OB_ADDR Value=OB_START  /initialise: first lux = OB_BASE
── Output sink (file descriptor written by PB_FLUSH) ─────────
//
Default = STDOUT (1). Code that needs to write somewhere else
(e.g. wane.re writing diagnostics to stderr) does:
Move El1=STDERR Exit=RA_OB_FD     switch
... emits ...                        EMIT_STR/INT/PUT_BYTE
Jump Exit=FLUSH                      drain
Move El1=STDOUT Exit=RA_OB_FD     restore
The EMIT machinery itself does not change.
//
SET RA_OB_FD Value=1

── Private register for EMIT_INT inner loop ─────────────────

── Packed string decode registers (used by EMIT_PACKED_STR) ──
//
RA_PR_WORD  — current packed u64 word being decoded
RA_PR_SHIFT — byte shift within current word (0, 8, 16, ..., 56)
Also used by EMIT_PREAM_ENTRY in yaku.re (same algorithm).
//
NEW RA_PR_WORD
NEW RA_PR_SHIFT

SET SF_I64MIN_STR Value="9223372036854775808"

/── Shared string constants (used across multiple arias) ─────
SET NL_STR Value="\n"

── Private save slots removed — RA_LINK is saved/restored automatically ──


── PUT_BYTE: write RA_BYTE into packed OB buffer ─────────────
//
Packed layout: 8 bytes per lux, little-endian.
  RA_OB_SHIFT — persistent bit counter within lux: 0, 8, 16, 24, 32, 40, 48, 56 then 0
  RA_OB_ADDR  — persistent lux address (OB_BASE + OB_POS>>3), updated on lux boundary
  RA_OB_POS   — byte position 0..OB_SIZE-1 (used for flush check and PB_FLUSH SC_A2)

Invariant: RA_OB_ADDR always points at the CURRENT lux (the one we write into).
RA_OB_SHIFT=0 means we are at the start of the current lux (first byte position).
Advance of RA_OB_ADDR happens in the hot path AFTER writing 8 bytes,
NOT in PB_ZERO_LUX — so that the very first PUT_BYTE writes into OB_BASE itself.

Hot path (7 of 8 calls): 11 steps.
New-lux path (1 of 8 calls): 13 steps (advance addr + clear + reset + write).
//
JZ PUT_BYTE El1=RA_OB_SHIFT Exit=PB_ZERO_LUX
/write byte into current lux using cached RA_OB_ADDR
NOVA PB_SHL Op=Left El1=RA_BYTE El2=RA_OB_SHIFT Exit=RA_FLAG
NOVA Op=Read El1=RA_OB_ADDR Exit=RA_TMP
NOVA Op=Add El1=RA_TMP El2=RA_FLAG Exit=RA_TMP
NOVA Op=Write El1=RA_OB_ADDR El2=RA_TMP
NOVA Op=Add El1=RA_OB_POS El2=C_1 Exit=RA_OB_POS
NOVA Op=Add El1=RA_OB_SHIFT El2=C_8 Exit=RA_OB_SHIFT

JEQ PB_SHFT64 El1=RA_OB_SHIFT El2=C_64 Exit=PB_ADV_LUX /shift reached 64 → move to next lux
JEQ PB_FULL El1=RA_OB_POS El2=OB_SIZE Exit=PB_FLUSH
RREDI PB_DONE

/PB_ADV_LUX: shift reached 64 → advance RA_OB_ADDR, check flush
NOLINK
NOVA PB_ADV_LUX Op=Add El1=RA_OB_ADDR El2=C_1 Exit=RA_OB_ADDR /advance to next lux
CLEAR PB_ADV_RSH El1=RA_OB_SHIFT /reset shift to 0
JEQ PB_ADV_FULL El1=RA_OB_POS El2=OB_SIZE Exit=PB_FLUSH
RREDI PB_ADV_DONE

/PB_ZERO_LUX: RA_OB_SHIFT==0 on entry — clear current lux and write first byte.
/RA_OB_ADDR already points at the correct lux (set by init or by PB_ADV_LUX).
/No advance here — the invariant is that RA_OB_ADDR is already correct.
NOLINK
NOVA PB_ZERO_LUX Op=Write El1=RA_OB_ADDR El2=C_0 /clear lux (lazy zero)
NOVA PBZ_GO Op=Jump Exit=PB_SHL /write byte at shift=0

── PB_FLUSH: drain OB to RA_OB_FD ────────────────────────────
//
RA_LINK is preserved across PB_FLUSH; its final JumpReg returns to
whoever set RA_LINK (PB_FULLJ caller or FLUSH caller). This is what
lets FLUSH be a one-instruction guard that tail-jumps here.
//
NOVA PB_FLUSH Op=Move El1=RA_OB_FD Exit=SC_A0
NOVA Op=Move El1=OB_START Exit=SC_A1
NOVA Op=Move El1=RA_OB_POS Exit=SC_A2
NOVA Op=Move El1=SYS_WRITE_PACKED Exit=SC_NR

NOVA PB_SC Op=Exire
CLEAR PB_RST El1=RA_OB_POS
CLEAR PB_RST_SH El1=RA_OB_SHIFT
NOVA PB_RST_ADDR Op=Move El1=OB_START Exit=RA_OB_ADDR /reset lux address
RREDI PB_RETF

── FLUSH: guarded drain ──────────────────────────────────────/
//
If buffer is empty → return immediately.
Otherwise tail-jump into PB_FLUSH (RA_LINK intact, so PB_FLUSH's final
JumpReg returns to FLUSH's caller). One source of the drain algorithm.
//
NOLINK
NOVA FLUSH Op=JumpIf El1=RA_OB_POS Exit=FL_DRAIN
RREDI FL_NOOP
NOVA FL_DRAIN Op=Jump Exit=PB_FLUSH

── EMIT_PACKED_STR / EMIT_STR_ENTRY ─────────────────────────
//
IN:  RA_TW_LUX = Lux ID of first packed word; RA_LINK = caller return
OUT: all bytes emitted via PUT_BYTE; RA_LINK restored automatically

ENCODING: each Lux word holds up to 8 ASCII bytes packed little-endian:
word = b0 | (b1<<8) | ... | (b7<<56)/
A Lux with word=0 terminates the string (NUL word).
A NUL byte (b=0) within a word also terminates (mid-word end).

PHILOSOPHY: A Lux knows only itself (its 64-bit word). There is no
"next neighbour" contract. This aria (EMIT_PACKED_STR) imposes the
reading interpretation: decode 8 bytes, advance by 1 Lux ID (sequential
allocation), stop on zero. The Lux doesn't know it's part of a string —
the aria decides that. A different aria could read the same Lux as a
number, a colour, a timestamp — no intrinsic type.

EMIT_STR_ENTRY is an alias entry-point for EMIT_PACKED_STR (same code).
The automatic call stack holds the caller's RA_LINK across this chain.
RA_PR_SHIFT, RA_PR_WORD are scratch.
//
NOLINK
SET EMIT_STR_ENTRY Value=EMIT_PACKED_STR  /alias/
NOVA EMIT_PACKED_STR Op=Read El1=RA_TW_LUX Exit=RA_PR_WORD
/word == 0 → end of string
JZ EPS_WORDZERO El1=RA_PR_WORD Exit=EPS_DONE
/decode bytes from current word
CLEAR EPS_SHIFT_INIT El1=RA_PR_SHIFT
/shift starts at 0; go directly to byte extraction (loop-back via EPS_BLB2 → EPS_EXTB)
NOVA EPS_EXTB Op=Right El1=RA_PR_WORD El2=RA_PR_SHIFT Exit=RA_BYTE
NOVA EPS_MASK Op=And El1=RA_BYTE El2=C_255 Exit=RA_BYTE
/byte == 0 → end of string (mid-word NUL)/
JZ EPS_BYTEZERO El1=RA_BYTE Exit=EPS_DONE
/emit byte then advance shift; loop back only if shift < 64
RVOCA EPS_PB Body=PUT_BYTE
NOVA EPS_ADVSHIFT Op=Add El1=RA_PR_SHIFT El2=C_8 Exit=RA_PR_SHIFT
JEQ EPS_BLB El1=RA_PR_SHIFT El2=C_64 Exit=EPS_NEXTWORD
NOVA EPS_BLB2 Op=Jump Exit=EPS_EXTB
/advance to next packed-string chunk (stride=1: consecutive luces, 8 bytes per lux)
NOVA EPS_NEXTWORD Op=Add El1=RA_TW_LUX El2=C_1 Exit=RA_TW_LUX
NOVA EPS_WLBE Op=Jump Exit=EMIT_PACKED_STR
RREDI EPS_DONE
── EMIT_INT_ENTRY: emit RA_TMP2 as signed decimal integer ───
//
IN:  RA_TMP2 = value to emit; RA_LINK = caller return
OUT: decimal digits emitted via PUT_BYTE; RA_LINK restored automatically
Clobbers: RA_TMP2, RA_TMP3, RA_TMP4, RA_DS_POS, RA_BYTE, RA_FLAG
//
NOLINK
JZ EMIT_INT_ENTRY El1=RA_TMP2 Exit=EI_ZERO
NOVA EI_NEG Op=Less El1=RA_TMP2 El2=C_0 Exit=RA_FLAG
NOVA EI_NEGJ Op=JumpIf El1=RA_FLAG Exit=EI_ISNEG
NOVA EI_POS Op=Move El1=RA_TMP2 Exit=RA_TMP3
CLEAR EI_INIT El1=RA_DS_POS
/Extract digits LSD-first into DS scratch via shared INT_TO_DS
RVOCA EI_ITD_R Body=INT_TO_DS
/Emit digits MSD-first
NOVA EI_EMIT Op=Sub El1=RA_DS_POS El2=C_1 Exit=RA_DS_POS
NOVA EI_RDADDR Op=Add El1=DS_START El2=RA_DS_POS Exit=RA_TMP4
NOVA EI_RDVAL Op=Read El1=RA_TMP4 Exit=RA_BYTE
RVOCA EI_EB2 Body=PUT_BYTE
JZ EI_EMIT_DONE El1=RA_DS_POS Exit=EI_DONE
NOVA EI_LB2 Op=Jump Exit=EI_EMIT
/Return (automatic stack restores RA_LINK)
RREDI EI_DONE
/Zero special case
NOVA EI_ZERO Op=Move El1=ASCII_0 Exit=RA_BYTE
/NOTE: RCALL_AT is a builder-class macro — wired by LOAD_MAIN (see BUGS.md Pattern N).
/Until LOAD_MAIN runs, EI_ZEROB2 and EI_I64MINR are op=0 (unwired).
RCALL_AT EI_ZEROB2 PUT_BYTE EI_DONE
/Negative: emit '-', negate, then emit digits
NOVA EI_ISNEG Op=Move El1=MINUS Exit=RA_BYTE
RVOCA EI_NEGPB2 Body=PUT_BYTE
NOVA EI_NEGNEG Op=Sub El1=C_0 El2=RA_TMP2 Exit=RA_TMP2
NOVA EI_NEGCK Op=Less El1=RA_TMP2 El2=C_0 Exit=RA_FLAG
NOVA EI_NEGCKJ Op=JumpIf El1=RA_FLAG Exit=EI_I64MIN
NOVA EI_NEGCONT Op=Jump Exit=EI_POS
/I64_MIN special case (negation of I64_MIN overflows)
NOVA EI_I64MIN Op=Move El1=SF_I64MIN_STR Exit=RA_TW_LUX
RCALL_AT EI_I64MINR EMIT_STR_ENTRY EI_DONE

── Self-referential SEALs ────────────────────────────────────
//
(RA_ES_LOOP_RET removed — EMIT_PACKED_STR uses RVOCA per byte, no fixed loop pointer)

── INT_TO_DS: digit extraction shared by EMIT_INT_ENTRY ──────
IN:  RA_TMP3 = positive nonzero value (not I64MIN)
RA_DS_POS = 0 (caller must initialise)
RA_LINK = caller return address
OUT: DS scratch filled with ASCII digits LSD-first; RA_DS_POS = digit count
Clobbers: RA_TMP, RA_TMP4, RA_FLAG
Returns via JumpReg RA_LINK.
//
NOVA INT_TO_DS Op=Rem El1=RA_TMP3 El2=C_10 Exit=RA_TMP
NOVA ITD_DIG Op=Add El1=RA_TMP El2=ASCII_0 Exit=RA_TMP
NOVA ITD_ADDR Op=Add El1=DS_START El2=RA_DS_POS Exit=RA_TMP4
NOVA ITD_WRITE Op=Write El1=RA_TMP4 El2=RA_TMP
NOVA ITD_INC Op=Add El1=RA_DS_POS El2=C_1 Exit=RA_DS_POS
NOVA ITD_DIV Op=Div El1=RA_TMP3 El2=C_10 Exit=RA_TMP3
JZ ITD_CHK El1=RA_TMP3 Exit=ITD_DONE
NOVA ITD_LB Op=Jump Exit=INT_TO_DS
RREDI ITD_DONE
