//aria/output.re — Output buffer, digit scratch, and output subroutines

── ARCHITECTURE ──────────────────────────────────────────────
One pipeline, one sink, one set of subroutines:
PUT_BYTE / EMIT_STR_ENTRY / EMIT_INT_ENTRY  — write into OB
PB_FLUSH                                    — drain OB to fd RA_OB_FD
FLUSH                                       — guarded PB_FLUSH

The destination file descriptor is parametric: RA_OB_FD.word is the fd
PB_FLUSH writes to. Default = STDOUT (1). Other code (shioreru.re) sets
it to STDERR (2) for the duration of its writes, FLUSHes between
boundaries, and restores it. The EMIT/PUT/FLUSH machinery does not
duplicate per destination — there is only one of each.

── OB (Output Buffer) ────────────────────────────────────────
OB_START: OB_SIZE consecutive Lux used as a byte buffer.
RA_OB_BASE.word = OB_START's Lux ID (set via SETREF).
RA_OB_POS tracks how many bytes are currently buffered.
When full (RA_OB_POS == OB_SIZE), PB_FLUSH writes to RA_OB_FD and resets.

── DS (Digit Scratch) ────────────────────────────────────────
DS_START: 20 consecutive Lux used by EMIT_INT_ENTRY for digit reversal.
RA_DS_BASE.word = DS_START's Lux ID.

── WHY SETREF FOR BASES ──────────────────────────────────────
BLOCK creates Lux whose word = 0 by default. Using the block name
directly as El1 would read word=0 → address 0 (NULL sentinel).
The SETREF'd register holds the actual Lux ID of the block start.

── REGISTER DISCIPLINE ──────────────────────────────────────
RA_LINK is saved/restored automatically by the call stack (Voca/Redi).
Both are private to output.re — callers must not use them directly.
Callers may freely use RA_RET2 for their own nested-call needs.

SYSCALL CONVENTION (used in PB_FLUSH):
Set els via: Move El1=VALUE Exit=SC_Ax
SC_* are self-referential (SETREF SC_X SC_X), so Python reads
mem[SC_X_ID] = SC_X.word = the value set by Move.

DEPENDENCY: aspects.re, core/constants.re, aria/ascii.re, runtime/regs.re,
aria/io.re (STDOUT default sink),
runtime/registers.re, target/linux_generic.re

Single source of truth for the OB byte capacity.
OB buffer uses packed u64 luces: 8 bytes per lux.
OB_SIZE = byte capacity (256). OB_LUCES = lux count (32).
RA_OB_POS counts bytes (0..255). Byte i lives in:
  lux:  RA_OB_BASE + (RA_OB_POS right-shift 3)
  shift: (RA_OB_POS AND 7) left-shift 3
OB_SIZE stays 256 so PB_FULL and SYS_WRITE SC_A2 remain byte-counts.
Interpreter reads packed luces via SYS_WRITE_PACKED syscall 103.//
NEWSET OB_SIZE 256
NEWSET OB_LUCES 32

BLOCK OB_START OB_LUCES
BLOCK DS_START 20

NEW RA_OB_BASE
NEW RA_OB_POS
NEW RA_OB_SHIFT   /persistent bit-shift counter within lux: 0,8,16,24,32,40,48,56 then reset
NEW RA_OB_ADDR    /persistent lux address = OB_BASE + (OB_POS >> 3); updated on new-lux and flush
NEW RA_DS_BASE
NEW RA_DS_POS
SETREF RA_OB_BASE OB_START
SETREF RA_OB_ADDR OB_START   /initialise: first lux = OB_BASE
SETREF RA_DS_BASE DS_START

── Output sink (file descriptor written by PB_FLUSH) ─────────
//Default = STDOUT (1). Code that needs to write somewhere else
(e.g. shioreru.re writing diagnostics to stderr) does:
Move El1=STDERR Exit=RA_OB_FD     switch
... emits ...                        EMIT_STR/INT/PUT_BYTE
Jump Exit=FLUSH                      drain
Move El1=STDOUT Exit=RA_OB_FD     restore
The EMIT machinery itself does not change.//
NEWSET RA_OB_FD 1

── Private register for EMIT_INT inner loop ─────────────────

── Packed string decode registers (used by EMIT_PACKED_STR) ──
//RA_PR_WORD  — current packed u64 word being decoded
RA_PR_SHIFT — byte shift within current word (0, 8, 16, ..., 56)
Also used by EMIT_PREAM_ENTRY in yaku.re (same algorithm).//
NEW RA_PR_WORD
NEW RA_PR_SHIFT

NEWSET SF_I64MIN_STR "9223372036854775808"

/── Shared string constants (used across multiple arias) ─────
NEWSET NL_STR "\n"

── Private save slots removed — RA_LINK is saved/restored automatically ──


── PUT_BYTE: write RA_BYTE into packed OB buffer ─────────────
//Packed layout: 8 bytes per lux, little-endian.
  RA_OB_SHIFT — persistent bit counter within lux: 0, 8, 16, 24, 32, 40, 48, 56 then 0
  RA_OB_ADDR  — persistent lux address (OB_BASE + OB_POS>>3), updated only on lux boundary
  RA_OB_POS   — byte position 0..OB_SIZE-1 (still used for flush check and PB_FLUSH SC_A2)
Hot path (7 of 8 calls): 11 steps.
New-lux path (1 of 8 calls): 14 steps.
Average ~11.4 steps vs old ~13.5.//
JZ PUT_BYTE RA_OB_SHIFT PB_ZERO_LUX
/write byte into current lux using cached RA_OB_ADDR
ITO PB_SHL      Left      El1=RA_BYTE     El2=RA_OB_SHIFT Exit=RA_FLAG  /shift byte into bit position
ITO PB_RD       Read      El1=RA_OB_ADDR  Exit=RA_TMP                   /read existing lux
ITO PB_OR       Add       El1=RA_TMP      El2=RA_FLAG    Exit=RA_TMP    /OR in shifted byte
ITO PB_STORE    Write     El1=RA_OB_ADDR  El2=RA_TMP                    /write back
ITO PB_INC      Add       El1=RA_OB_POS   El2=C_1        Exit=RA_OB_POS /pos++
ITO PB_SHFT     Add       El1=RA_OB_SHIFT El2=C_8        Exit=RA_OB_SHIFT /advance bit counter
JEQ PB_FULL RA_OB_POS OB_SIZE PB_FLUSH
RREDI PB_DONE

/PB_ZERO_LUX: first byte of new lux — advance RA_OB_ADDR, clear new lux, reset shift
NOLINK
ITO PB_ZERO_LUX Add  El1=RA_OB_ADDR  El2=C_1        Exit=RA_OB_ADDR  /advance to next lux
ITO PBZ_CLR      Write El1=RA_OB_ADDR El2=C_0                         /clear it (lazy zero)
CLEAR PBZ_RSH RA_OB_SHIFT
ITO PBZ_GO       Jump  Exit=PB_SHL                                     /continue with write

── PB_FLUSH: drain OB to RA_OB_FD ────────────────────────────
//RA_LINK is preserved across PB_FLUSH; its final JumpReg returns to
whoever set RA_LINK (PB_FULLJ caller or FLUSH caller). This is what
lets FLUSH be a one-instruction guard that tail-jumps here.//
ITO PB_FLUSH    Move    El1=RA_OB_FD          Exit=SC_A0
ITO PB_FX1      Move    El1=RA_OB_BASE        Exit=SC_A1
ITO PB_FX2      Move    El1=RA_OB_POS         Exit=SC_A2
ITO PB_SETNR    Move    El1=SYS_WRITE_PACKED  Exit=SC_NR
ITO PB_SC       Exire
CLEAR PB_RST RA_OB_POS
CLEAR PB_RST_SH RA_OB_SHIFT
ITO PB_RST_ADDR Move    El1=RA_OB_BASE        Exit=RA_OB_ADDR   /reset lux address
RREDI PB_RETF

── FLUSH: guarded drain ──────────────────────────────────────/
//If buffer is empty → return immediately.
Otherwise tail-jump into PB_FLUSH (RA_LINK intact, so PB_FLUSH's final
JumpReg returns to FLUSH's caller). One source of the drain algorithm.//
NOLINK
ITO FLUSH       JumpIf  El1=RA_OB_POS   Exit=FL_DRAIN
RREDI FL_NOOP
ITO FL_DRAIN    Jump    Exit=PB_FLUSH

── EMIT_PACKED_STR / EMIT_STR_ENTRY ─────────────────────────
//IN:  RA_TW_LUX = Lux ID of first packed word; RA_LINK = caller return
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
RA_PR_SHIFT, RA_PR_WORD are scratch.//
NOLINK
NEWREF EMIT_STR_ENTRY EMIT_PACKED_STR  /alias/
ITO EMIT_PACKED_STR Read El1=RA_TW_LUX   Exit=RA_PR_WORD
/word == 0 → end of string
JZ EPS_WORDZERO RA_PR_WORD EPS_DONE
/decode bytes from current word
CLEAR EPS_SHIFT_INIT RA_PR_SHIFT
/shift starts at 0; go directly to byte extraction (loop-back via EPS_BLB2 → EPS_EXTB)
ITO EPS_EXTB      Right       El1=RA_PR_WORD   El2=RA_PR_SHIFT Exit=RA_BYTE
ITO EPS_MASK      And       El1=RA_BYTE      El2=C_255      Exit=RA_BYTE
/byte == 0 → end of string (mid-word NUL)/
JZ EPS_BYTEZERO RA_BYTE EPS_DONE
/emit byte then advance shift; loop back only if shift < 64
RVOCA EPS_PB PUT_BYTE
ITO EPS_ADVSHIFT  Add       El1=RA_PR_SHIFT  El2=C_8        Exit=RA_PR_SHIFT
JEQ EPS_BLB RA_PR_SHIFT C_64 EPS_NEXTWORD
ITO EPS_BLB2      Jump      Exit=EPS_EXTB
/advance to next packed-string chunk (DATA_LUX_MIN=2 luces: [word, 0])
ITO EPS_NEXTWORD  Add       El1=RA_TW_LUX   El2=C_2        Exit=RA_TW_LUX
ITO EPS_WLBE      Jump      Exit=EMIT_PACKED_STR
RREDI EPS_DONE
── EMIT_INT_ENTRY: emit RA_TMP2 as signed decimal integer ───
//IN:  RA_TMP2 = value to emit; RA_LINK = caller return
OUT: decimal digits emitted via PUT_BYTE; RA_LINK restored automatically
Clobbers: RA_TMP2, RA_TMP3, RA_TMP4, RA_DS_POS, RA_BYTE, RA_FLAG//
NOLINK
JZ EMIT_INT_ENTRY RA_TMP2 EI_ZERO
ITO EI_NEG      Less El1=RA_TMP2     El2=C_0        Exit=RA_FLAG
ITO EI_NEGJ     JumpIf    El1=RA_FLAG     Exit=EI_ISNEG
ITO EI_POS      Move      El1=RA_TMP2     Exit=RA_TMP3
CLEAR EI_INIT RA_DS_POS
/Extract digits LSD-first into DS scratch via shared INT_TO_DS
RVOCA EI_ITD_R INT_TO_DS
/Emit digits MSD-first
ITO EI_EMIT     Sub       El1=RA_DS_POS   El2=C_1        Exit=RA_DS_POS
ITO EI_RDADDR   Add       El1=RA_DS_BASE  El2=RA_DS_POS  Exit=RA_TMP4
ITO EI_RDVAL    Read   El1=RA_TMP4     Exit=RA_BYTE
RVOCA EI_EB2 PUT_BYTE
JZ EI_EMIT_DONE RA_DS_POS EI_DONE
ITO EI_LB2      Jump      Exit=EI_EMIT
/Return (automatic stack restores RA_LINK)
RREDI EI_DONE
/Zero special case
ITO EI_ZERO     Move      El1=ASCII_0     Exit=RA_BYTE
/NOTE: RCALL_AT is a builder-class macro — wired by LOAD_MAIN (see BUGS.md Pattern N).
/Until LOAD_MAIN runs, EI_ZEROB2 and EI_I64MINR are op=0 (unwired).
RCALL_AT EI_ZEROB2 PUT_BYTE EI_DONE
/Negative: emit '-', negate, then emit digits
ITO EI_ISNEG    Move      El1=MINUS       Exit=RA_BYTE
RVOCA EI_NEGPB2 PUT_BYTE
ITO EI_NEGNEG   Sub       El1=C_0         El2=RA_TMP2    Exit=RA_TMP2
ITO EI_NEGCK    Less El1=RA_TMP2     El2=C_0        Exit=RA_FLAG
ITO EI_NEGCKJ   JumpIf    El1=RA_FLAG     Exit=EI_I64MIN
ITO EI_NEGCONT  Jump      Exit=EI_POS
/I64_MIN special case (negation of I64_MIN overflows)
ITO EI_I64MIN   Move      El1=SF_I64MIN_STR Exit=RA_TW_LUX
RCALL_AT EI_I64MINR EMIT_STR_ENTRY EI_DONE

── Self-referential SETREFs ──────────────────────────────────
//(RA_ES_LOOP_RET removed — EMIT_PACKED_STR uses RVOCA per byte, no fixed loop pointer)

── INT_TO_DS: digit extraction shared by EMIT_INT_ENTRY ──────
IN:  RA_TMP3 = positive nonzero value (not I64MIN)
RA_DS_POS = 0 (caller must initialise)
RA_LINK = caller return address
OUT: DS scratch filled with ASCII digits LSD-first; RA_DS_POS = digit count
Clobbers: RA_TMP, RA_TMP4, RA_FLAG
Returns via JumpReg RA_LINK.//
NOLINK
ITO INT_TO_DS   Rem       El1=RA_TMP3    El2=C_10       Exit=RA_TMP
ITO ITD_DIG     Add       El1=RA_TMP     El2=ASCII_0    Exit=RA_TMP
ITO ITD_ADDR    Add       El1=RA_DS_BASE El2=RA_DS_POS  Exit=RA_TMP4
ITO ITD_WRITE   Write  El1=RA_TMP4    El2=RA_TMP
ITO ITD_INC     Add       El1=RA_DS_POS  El2=C_1        Exit=RA_DS_POS
ITO ITD_DIV     Div       El1=RA_TMP3    El2=C_10       Exit=RA_TMP3
JZ ITD_CHK RA_TMP3 ITD_DONE
ITO ITD_LB      Jump      Exit=INT_TO_DS
RREDI ITD_DONE