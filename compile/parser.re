//============================================================
parser.re — Reca runtime .re parser  (PS_MAIN entry point)

ARCHITECTURE FIX: All JumpIf and Jump luxes are now ITO-based.
The old pattern (LINK X Op JumpIf + LINK PREV Next X at bottom)
was broken because _args() uses "last-overwrite wins" (oldest
lumen), so the explicit LINK overrides were always dead.  The
auto-Next lumen (set when the following ITO was processed) is
the OLDEST and always wins.  ITO-based JumpIf gets its Next via
auto-link to the following instruction, which is correct.

REMOVED: PS_LK_BQ / PS_LK_VS fast-paths.  PS_BQ_BASE=0 and
PS_VS_BASE=0 at load time → those paths always computed the wrong
Lux ID.  General PBKT/LSYM lookup handles any remaining names.

DEPENDS ON: aspects.re, core/constants.re, aria/ascii.re,
aria/math.re, aria/io.re, runtime/registers.re,
target/linux_generic.re, runtime/regs.re, aria/output.re//
============================================================

── Data buffers ─────────────────────────────────────────────
BLOCK PBKT_000 256
BLOCK LSYM_000 512
BLOCK PLBUF_000 256
BLOCK PTBUF_000 256


── Parser state registers ────────────────────────────────────
NEW PR_READFD        /current read fd (0=stdin)
NEW PR_SAVED_READFD  /saved outer fd for nested LOAD
NEW PR_SAVED_EOF     /saved outer eof flag
NEW PR_LOAD_FD       /fd returned by openat
NEW PR_BYTE          /current input byte
NEW PR_EOF           /eof flag (0=live, 1=eof)
NEW PR_LPOS          /current position in line buffer
NEW PR_LLEN          /length of current line
NEW PR_TLEN          /token length
NEW PR_HASH          /token hash (32-bit djb2)
NEW PR_SYM           /symbol lookup result (Lux ID, 0=not found)
NEW PR_EL1          /element 1 (src/name)
NEW PR_EL2          /element 2 (rel/key)
NEW PR_EL3          /element 3 (exit_lux/val)
NEW PR_VAL           /parsed integer value
NEW PR_D3_OFF        /3-digit parse: offset into token buffer
NEW PR_D3_VAL        /3-digit parse: result
NEW PR_TMP           /scratch 1
NEW PR_TMP2          /scratch 2
NEW PR_TMP3          /scratch 3
NEW PR_TMP4          /scratch 4
NEW PR_LBUF          /line buffer base Lux ID  (set at runtime)
NEW PR_TBUF          /token buffer base Lux ID (set at runtime)
NEW PR_PBKT          /PBKT base Lux ID
NEW PR_LSYM          /LSYM base Lux ID

── Auto-Next state ──────────────────────────────────────────
//PR_LAST_INSTR: ID of last instruction Lux created.
0 = no previous instruction (NOLINK state).
Set to PR_AN_LAST after each instruction-creating command.
PR_AN_FIRST: first Lux of the current instruction sequence
(receives incoming Next lumen from PR_LAST_INSTR).
PR_AN_LAST: last Lux of the current instruction sequence
(stored into PR_LAST_INSTR for the next command).
All instruction-creating handlers set PR_AN_FIRST + PR_AN_LAST
then Jump → PS_INSTR_DONE (not PS_LINE_DONE directly).
NOLINK sets PR_LAST_INSTR = 0 and Jumps → PS_LINE_DONE directly.//
NEW PR_LAST_INSTR
NEW PR_AN_FIRST
NEW PR_AN_LAST

── Return-address holders (word set via SETREF at bottom) ────

── Per-subroutine return registers ───────────────────────────

── PS_LK_PREFIX3 registers ───────────────────────────────────
NEW PR_PFX_CHAR      /expected char at tbuf[1] ('B' or 'S')/
NEW PR_PFX_BASE      /base value (word of RA_OB_BASE or RA_DS_BASE)/

── Constants ─────────────────────────────────────────────────
//AT_FDCWD, O_RDONLY, SYS_CLOSE are shared system constants and live in
aria/io.re and target/linux_generic.re. parser.re uses them
directly — no PS_-prefixed shadow copies.//
NEW PS_PBKT_BASE
NEW PS_LSYM_BASE
NEW PS_LBUF_BASE
NEW PS_TBUF_BASE

SETREF PS_PBKT_BASE PBKT_000
SETREF PS_LSYM_BASE LSYM_000
SETREF PS_LBUF_BASE PLBUF_000
SETREF PS_TBUF_BASE PTBUF_000

── Pre-computed PBKT hash table entries ──────────────────────
/Packed as (hash32 << 32) | lux_id for common tokens.
SET PBKT_011 1009734351589474367
SET PBKT_012 12835984667219853353
SET PBKT_013 1009734355884441664
SET PBKT_014 1009734360179408961
SET PBKT_015 1009734364474376258
SET PBKT_019 5721439678620500019
SET PBKT_029 558466269557293108
SET PBKT_031 11136537795491790896
SET PBKT_034 12835983662197506088
SET PBKT_037 830924185768296483
SET PBKT_040 830881317699715106
SET PBKT_047 782267540653998089
SET PBKT_050 1017379423441125423
SET PBKT_053 8972320774517948422
SET PBKT_057 18428771701455061046
SET PBKT_060 8973601735629078545
SET PBKT_063 12835984886263185451
SET PBKT_065 14945643143834370055
SET PBKT_076 15198497880118657029
SET PBKT_095 6697981152851918862
SET PBKT_101 8973599712699482158
SET PBKT_103 12835983958550249514
SET PBKT_111 830953090898198560
SET PBKT_118 830928931707158567
SET PBKT_124 8973757041646501933
SET PBKT_128 11426633359976038408
SET PBKT_129 1980029676357156880
SET PBKT_144 8971916545080950787
SET PBKT_145 8971916549375918084
SET PBKT_147 8973573521988911153
SET PBKT_164 8973864965584715778
SET PBKT_174 830866500062543882
SET PBKT_193 8973320831883018284
SET PBKT_204 830951291306901540
SET PBKT_206 8972918466461827087
SET PBKT_208 13618781712916938765
SET PBKT_210 830951317076705317
SET PBKT_214 15431883009959133234
SET PBKT_218 1009736340159332414
SET PBKT_228 25179795528613889
SET PBKT_230 25179804118548518
SET PBKT_233 558475941823643701
SET PBKT_243 830925070531559457
SET PBKT_248 830867917401751563
SET PBKT_254 830975695311077388

============================================================/
//PS_RB — read one byte from PR_READFD into PR_BYTE
Sets PR_EOF=1 on eof.  Returns via JumpReg PR_RB_RET.//
============================================================/
ITO PS_RB     Move    El1=PR_READFD  Exit=SC_A0
ITO PS_RB_X1  Move    El1=PR_LBUF    Exit=SC_A1
ITO PS_RB_X2  Move    El1=C_1        Exit=SC_A2
ITO PS_RB_X8  Move    El1=SYS_READ   Exit=SC_NR
ITO PS_RB_SC  Exire
ITO PS_RB_EZ  Equal El1=SC_A0 El2=C_0 Exit=PR_EOF
ITO PS_RB_LD  Read El1=PR_LBUF    Exit=PR_BYTE
RREDI PS_RB_RJ

============================================================
//PS_RL — read one line into LBUF[0..PR_LLEN-1]
Skips CR, stops at LF or EOF.  Returns via JumpReg PR_RL_RET.//
/============================================================
CLEAR PS_RL PR_LPOS
/PS_RL_H: check EOF
JEQ PS_RL_H PR_EOF C_1 PS_RL_DONE
/call PS_RB
RVOCA CG_RL_RB  PS_RB
JEQ RET_RL_RB_EK PR_EOF C_1 PS_RL_DONE
/check LF
JEQ PS_RL_NL PR_BYTE LF PS_RL_DONE
/check CR — skip it, loop back
JEQ PS_RL_CR PR_BYTE CR PS_RL_H
/check overflow (lpos < 255)
ITO PS_RL_OV  Less El1=PR_LPOS El2=C_255 Exit=PR_TMP
ITO PS_RL_OVJ JumpIf  El1=PR_TMP   Exit=PS_RL_ST
/overflow: don't store, still increment pos
ITO PS_RL_NOSTORE Add El1=PR_LPOS  El2=C_1   Exit=PR_LPOS
ITO PS_RL_NOLB    Jump Exit=PS_RL_H
/store byte
ITO PS_RL_ST  Add     El1=PR_LBUF  El2=PR_LPOS Exit=PR_TMP
ITO PS_RL_WR  Write El1=PR_TMP  El2=PR_BYTE
ITO PS_RL_PI  Add     El1=PR_LPOS  El2=C_1     Exit=PR_LPOS
ITO PS_RL_LB  Jump    Exit=PS_RL_H
ITO PS_RL_DONE Move   El1=PR_LPOS  Exit=PR_LLEN
CLEAR PS_RL_LP0 PR_LPOS
RREDI PS_RL_RJ

============================================================
//PS_NT — scan next token from line buffer into TBUF
Skips leading whitespace; stops at SP/TAB/=/NUL; computes hash.
Leaf. Returns via Redi (RA_LINK). Voca with RVOCA.//
============================================================
ITO PS_NT     Move    El1=C_5381    Exit=PR_HASH
CLEAR PS_NT_TL PR_TLEN
/skip leading whitespace
JEQ PS_NT_SS PR_LPOS PR_LLEN PS_NT_DONE
ITO PS_NT_SSR Add     El1=PR_LBUF  El2=PR_LPOS  Exit=PR_TMP
ITO PS_NT_SLD Read El1=PR_TMP    Exit=PR_BYTE
JEQ PS_NT_SPC PR_BYTE SP PS_NT_SKP
JEQ PS_NT_TAB PR_BYTE TAB PS_NT_SKP
/not whitespace — start reading token body
JEQ PS_NT_BODY PR_LPOS PR_LLEN PS_NT_DONE
ITO PS_NT_BRD Add     El1=PR_LBUF  El2=PR_LPOS  Exit=PR_TMP
ITO PS_NT_BLD Read El1=PR_TMP    Exit=PR_BYTE
SWITCH PR_BYTE
    SP     PS_NT_DONE
    TAB    PS_NT_DONE
    EQUALS PS_NT_DONE
    0      PS_NT_DONE
/update hash: hash = hash*33 + byte
ITO PS_NT_BH  Mul     El1=PR_HASH  El2=C_33    Exit=PR_HASH
ITO PS_NT_BHA Add     El1=PR_HASH  El2=PR_BYTE  Exit=PR_HASH
/store byte in token buffer (if not overflow)
ITO PS_NT_BOV Less El1=PR_TLEN El2=C_255  Exit=PR_TMP
ITO PS_NT_BOVJ JumpIf El1=PR_TMP   Exit=PS_NT_ST
ITO PS_NT_BSKP Add   El1=PR_LPOS  El2=C_1       Exit=PR_LPOS
ITO PS_NT_BSKLB Jump  Exit=PS_NT_BODY
ITO PS_NT_ST  Add     El1=PR_TBUF  El2=PR_TLEN   Exit=PR_TMP
ITO PS_NT_TWR Write El1=PR_TMP   El2=PR_BYTE
ITO PS_NT_TPI Add     El1=PR_TLEN  El2=C_1        Exit=PR_TLEN
ITO PS_NT_LPI Add     El1=PR_LPOS  El2=C_1        Exit=PR_LPOS
ITO PS_NT_LB  Jump    Exit=PS_NT_BODY
/done: NUL-terminate, mask hash to 32 bits
ITO PS_NT_DONE Add    El1=PR_TBUF  El2=PR_TLEN    Exit=PR_TMP
ITO PS_NT_NWR Write El1=PR_TMP  El2=C_0
ITO PS_NT_HM  And     El1=PR_HASH  El2=MASK_LOW32 Exit=PR_HASH
RREDI PS_NT_RJ
/whitespace skip increment (jumps back to leading-ws check)
ITO PS_NT_SKP Add     El1=PR_LPOS  El2=C_1        Exit=PR_LPOS
ITO PS_NT_SKB Jump    Exit=PS_NT_SS

============================================================
//PS_D3 — parse 3 ASCII digits at tbuf[PR_D3_OFF..+2] → PR_D3_VAL
Returns via JumpReg PR_D3_RET.//
============================================================
ITO PS_D3     Add     El1=PR_TBUF  El2=PR_D3_OFF  Exit=PR_TMP
ITO PS_D3_R0  Read El1=PR_TMP   Exit=PR_TMP2
ITO PS_D3_S0  Sub     El1=PR_TMP2  El2=ASCII_0    Exit=PR_TMP2
ITO PS_D3_M0  Mul     El1=PR_TMP2  El2=C_100      Exit=PR_D3_VAL
ITO PS_D3_A1  Add     El1=PR_D3_OFF El2=C_1       Exit=PR_TMP3
ITO PS_D3_P1  Add     El1=PR_TBUF  El2=PR_TMP3    Exit=PR_TMP
ITO PS_D3_R1  Read El1=PR_TMP   Exit=PR_TMP2
ITO PS_D3_S1  Sub     El1=PR_TMP2  El2=ASCII_0    Exit=PR_TMP2
ITO PS_D3_M1  Mul     El1=PR_TMP2  El2=C_10       Exit=PR_TMP2  /×10 for decimal
ITO PS_D3_A1V Add     El1=PR_D3_VAL El2=PR_TMP2   Exit=PR_D3_VAL
ITO PS_D3_A2  Add     El1=PR_D3_OFF El2=C_2       Exit=PR_TMP3
ITO PS_D3_P2  Add     El1=PR_TBUF  El2=PR_TMP3    Exit=PR_TMP
ITO PS_D3_R2  Read El1=PR_TMP   Exit=PR_TMP2
ITO PS_D3_S2  Sub     El1=PR_TMP2  El2=ASCII_0    Exit=PR_TMP2
ITO PS_D3_A2V Add     El1=PR_D3_VAL El2=PR_TMP2   Exit=PR_D3_VAL
RREDI PS_D3_RJ

============================================================
//PS_INT — parse token buffer as decimal integer → PR_VAL
Returns via JumpReg PR_INT_RET.//
============================================================
CLEAR PS_INT PR_VAL
CLEAR PS_INT_I PR_TMP
JEQ PS_INT_LP PR_TMP PR_TLEN PS_INT_DONE
ITO PS_INT_RD Add     El1=PR_TBUF  El2=PR_TMP     Exit=PR_TMP2
ITO PS_INT_LD Read El1=PR_TMP2  Exit=PR_BYTE
ITO PS_INT_DL Less El1=PR_BYTE El2=ASCII_0   Exit=PR_TMP2
ITO PS_INT_DLJ JumpIf El1=PR_TMP2  Exit=PS_INT_DONE
ITO PS_INT_DH Greater El1=PR_BYTE El2=ASCII_9   Exit=PR_TMP2
ITO PS_INT_DHJ JumpIf El1=PR_TMP2  Exit=PS_INT_DONE
ITO PS_INT_DIG Sub    El1=PR_BYTE  El2=ASCII_0    Exit=PR_BYTE
ITO PS_INT_MUL Mul    El1=PR_VAL   El2=C_10       Exit=PR_TMP2
ITO PS_INT_ADD Add    El1=PR_TMP2  El2=PR_BYTE    Exit=PR_VAL
ITO PS_INT_II Add     El1=PR_TMP   El2=C_1        Exit=PR_TMP
ITO PS_INT_LB Jump    Exit=PS_INT_LP
CLEAR PS_INT_DONE RA_TMP
RREDI PS_INT_RJ

============================================================
//PS_LK — look up token (PR_HASH, PR_TBUF) → PR_SYM
Fast paths: OB_NNN (→ RA_OB_BASE + N), DS_NNN (→ RA_DS_BASE + N)
General paths: PBKT hash table, then LSYM hash table.
Returns PR_SYM=0 if not found.  Returns via JumpReg PR_LK_RET.//
============================================================
ITO PS_LK Read El1=PR_TBUF Exit=PR_TMP   /first byte

── OB_ fast path: "OB_NNN" → RA_OB_BASE + NNN ───────────────
JEQ PS_LK_O PR_TMP ASCII_O PS_LK_OB1
/fall-through: not 'O', try DS path (PR_TMP still holds first byte)
JEQ PS_LK_DD PR_TMP ASCII_D PS_LK_DS1
/fall-through: not 'O' or 'D', go to PBKT general lookup
ITO PS_LK_PBKT Move     El1=PR_PBKT    Exit=RA_HT_BASE
ITO PS_LK_PHS  Move     El1=PR_HASH    Exit=RA_HT_HASH
ITO PS_LK_PSZ  Move     El1=C_256      Exit=RA_HT_SIZE
ITO PS_LK_PMK  Move     El1=C_255      Exit=RA_HT_MASK
RVOCA PS_LK_PRJ HT_LOOKUP
JZ PS_LK_PBKT_DONE RA_HT_RESULT PS_LK_LSYM
/PBKT hit
ITO PS_LK_PFND Move     El1=RA_HT_RESULT Exit=PR_SYM
RREDI PS_LK_PFRJ
/LSYM lookup via HT_LOOKUP
ITO PS_LK_LSYM Move     El1=PR_LSYM    Exit=RA_HT_BASE
ITO PS_LK_LHS  Move     El1=PR_HASH    Exit=RA_HT_HASH
ITO PS_LK_LSZ  Move     El1=C_512      Exit=RA_HT_SIZE
ITO PS_LK_LMK  Sub      El1=C_512      El2=C_1 Exit=RA_HT_MASK
RVOCA PS_LK_LRJ HT_LOOKUP
ITO PS_LK_LSYM_DONE Move El1=RA_HT_RESULT Exit=PR_SYM
RREDI PS_LK_LFRJ

/OB_ path: "OB_NNN" → RA_OB_BASE + NNN via PS_LK_PREFIX3
ITO PS_LK_OB1  Read El1=RA_OB_BASE Exit=PR_PFX_BASE
ITO PS_LK_OBM  Move    El1=ASCII_B    Exit=PR_PFX_CHAR
RCALL_AT PS_LK_OBR PS_LK_PREFIX3 PS_LK_OBD
JZ PS_LK_OBD PR_SYM PS_LK_PBKT
RREDI PS_LK_OBRJ

/DS_ path: "DS_NNN" → RA_DS_BASE + NNN via PS_LK_PREFIX3
ITO PS_LK_DS1  Read El1=RA_DS_BASE Exit=PR_PFX_BASE
ITO PS_LK_DSM  Move    El1=ASCII_S    Exit=PR_PFX_CHAR
RCALL_AT PS_LK_DSR PS_LK_PREFIX3 PS_LK_DSD
JZ PS_LK_DSD PR_SYM PS_LK_PBKT
RREDI PS_LK_DSRJ

============================================================
//PS_INS — insert (PR_HASH, PR_EL1=lux_id) into LSYM via HT_INSERT
Returns via JumpReg PR_INS_RET.//
============================================================
ITO PS_INS     Move    El1=PR_LSYM    Exit=RA_HT_BASE
ITO PS_INS_HS  Move    El1=PR_HASH    Exit=RA_HT_HASH
ITO PS_INS_LI  Move    El1=PR_EL1    Exit=RA_HT_LID
ITO PS_INS_SZ  Move    El1=C_512      Exit=RA_HT_SIZE
ITO PS_INS_MK  Sub     El1=C_512      El2=C_1 Exit=RA_HT_MASK
RVOCA PS_INS_RJ HT_INSERT

============================================================
/PS_CMD_NEW — create a new Lux, insert into LSYM
============================================================
RVOCA PS_CMD_NEW ALLOC_LUX
ITO PS_NW_SETLID Move  El1=RA_ALLOC_RESULT Exit=PR_EL1
RVOCA CG_NW_INS  PS_INS
RREDI RET_NW_INS_JR

============================================================
//PS_CMD_SET — SET name value
After name lookup: if next non-space char is '"' → string SET (packed chain)
else → read token as integer (existing path)//
/============================================================
NEW RC_ST_STR_LOOP  /return slot for string reader inner loop
NEW PR_ST_WORD      /current packed word being built
NEW PR_ST_SHIFT     /bit shift within current word (0,8,16,...,56)
NEW PR_ST_FIRST     /first packed Lux ID
NEW PR_ST_PREV      /previously allocated packed Lux (for chain if needed)
NEW PR_ST_BYTE      /current string byte

RVOCA PS_CMD_SET PS_LK
ITO RET_ST_LK1_MV Move El1=PR_SYM     Exit=PR_EL1
//peek at current line buffer char (after name token, lpos already past name)
skip spaces to find value start//
JEQ PS_ST_SKIP PR_LPOS PR_LLEN PS_ST_INT_PATH
ITO PS_ST_PEEKR Add      El1=PR_LBUF  El2=PR_LPOS Exit=PR_TMP
ITO PS_ST_PEEKV Read  El1=PR_TMP   Exit=PR_TMP
JEQ PS_ST_SPCCK PR_TMP SP PS_ST_SKIPSP
JEQ PS_ST_TABCK PR_TMP TAB PS_ST_SKIPSP
/not space: check for '"'
JEQ PS_ST_DQCHK PR_TMP DQUOTE PS_ST_STR_START
ITO PS_ST_INT_PATH Jump   Exit=PS_ST_INT_READ
ITO PS_ST_SKIPSP Add     El1=PR_LPOS  El2=C_1      Exit=PR_LPOS
ITO PS_ST_SKIPLB Jump    Exit=PS_ST_SKIP

── Integer SET (existing path) ───────────────────────────────
CLEAR PS_ST_INT_READ RA_TMP
RVOCA CG_ST_NT   PS_NT
RVOCA CG_ST_PI  PS_INT
ITO PS_ST_WR   Write El1=PR_EL1   El2=PR_VAL
RREDI PS_ST_RJ

── String SET: packed u64 encoding ──────────────────────────
//lpos currently points at '"'. Skip it, then read bytes until closing '"'.
Pack 8 bytes per Lux word (little-endian). Terminate with NUL-word Lux.
Store first Lux ID into PR_EL1 (the named Lux).//
ITO PS_ST_STR_START Add  El1=PR_LPOS  El2=C_1      Exit=PR_LPOS  /skip opening '"'
CLEAR PS_ST_STR_INIT PR_ST_WORD
CLEAR PS_ST_STR_INITS PR_ST_SHIFT
CLEAR PS_ST_STR_INITF PR_ST_FIRST
── string byte loop ─────────────────────────────────────────
JEQ PS_ST_SLOOP PR_LPOS PR_LLEN PS_ST_STR_FLUSH
ITO PS_ST_SREAD Add     El1=PR_LBUF  El2=PR_LPOS Exit=PR_TMP
ITO PS_ST_SLOAD Read El1=PR_TMP   Exit=PR_ST_BYTE
/check closing '"'
JEQ PS_ST_DQEND PR_ST_BYTE DQUOTE PS_ST_STR_FLUSH
ITO PS_ST_SINCL Add     El1=PR_LPOS  El2=C_1      Exit=PR_LPOS
/handle \ escape: check if byte is '\' (92=BACKSLASH)
JEQ PS_ST_BSCK PR_ST_BYTE BACKSLASH PS_ST_ESCAPE
ITO PS_ST_PACKB Jump    Exit=PS_ST_PACK_BYTE
/escape handler: read next char (if end of line, flush)
JEQ PS_ST_ESCAPE PR_LPOS PR_LLEN PS_ST_STR_FLUSH
ITO PS_ST_ESCRD  Add     El1=PR_LBUF El2=PR_LPOS  Exit=PR_TMP
ITO PS_ST_ESCLD  Read El1=PR_TMP  Exit=PR_ST_BYTE
ITO PS_ST_ESCINCL Add   El1=PR_LPOS El2=C_1       Exit=PR_LPOS
/'n'(110) → LF(10): byte - ASCII_a == 13
ITO PS_ST_ESCNCK Sub    El1=PR_ST_BYTE El2=ASCII_a Exit=PR_TMP
JEQ PS_ST_ESC_N PR_TMP C_13 PS_ST_ESC_LF
/'t'(116) → TAB(9): byte - ASCII_a == 19
JEQ PS_ST_ESC_T PR_TMP C_19 PS_ST_ESC_TAB
/'\\' → BACKSLASH(92)
JEQ PS_ST_ESC_BS PR_ST_BYTE BACKSLASH PS_ST_PACK_BYTE
/unknown escape → pass byte through
ITO PS_ST_ESC_PASS Jump Exit=PS_ST_PACK_BYTE
ITO PS_ST_ESC_LF  Move  El1=LF  Exit=PR_ST_BYTE
ITO PS_ST_ESC_LFJMP Jump Exit=PS_ST_PACK_BYTE
ITO PS_ST_ESC_TAB Move  El1=TAB Exit=PR_ST_BYTE
── pack byte into current word ───────────────────────────────
ITO PS_ST_PACK_BYTE Left    El1=PR_ST_BYTE El2=PR_ST_SHIFT Exit=PR_TMP
ITO PS_ST_PBOR   Add    El1=PR_ST_WORD El2=PR_TMP   Exit=PR_ST_WORD
ITO PS_ST_PBINCS Add    El1=PR_ST_SHIFT El2=C_8     Exit=PR_ST_SHIFT
/if shift == 64 → word full, flush to Lux
JEQ PS_ST_PBFCK PR_ST_SHIFT C_64 PS_ST_FLUSH_WORD
ITO PS_ST_PBLB   Jump   Exit=PS_ST_SLOOP
── flush current word to a new Lux ──────────────────────────
RVOCA PS_ST_FLUSH_WORD ALLOC_LUX
/set word value
ITO PS_ST_FW_SET Write El1=RA_ALLOC_RESULT El2=PR_ST_WORD
/if first==0, this is the first Lux
JZ PS_ST_FW_FCK PR_ST_FIRST PS_ST_FW_SAVEFIRST
ITO PS_ST_FW_CONT Jump   Exit=PS_ST_FW_RESET
ITO PS_ST_FW_SAVEFIRST Move El1=RA_ALLOC_RESULT Exit=PR_ST_FIRST
/reset word accumulator
CLEAR PS_ST_FW_RESET PR_ST_WORD
CLEAR PS_ST_FW_RESETS PR_ST_SHIFT
ITO PS_ST_FW_LB   Jump   Exit=PS_ST_SLOOP
── flush partial word + NUL terminator ──────────────────────
/if PR_ST_SHIFT > 0: flush partial word
JZ PS_ST_STR_FLUSH PR_ST_SHIFT PS_ST_SF_NUL
/flush partial
RVOCA PS_ST_SF_ALLOC ALLOC_LUX
ITO PS_ST_SF_SET  Write El1=RA_ALLOC_RESULT El2=PR_ST_WORD
JZ PS_ST_SF_FCK PR_ST_FIRST PS_ST_SF_SF
ITO PS_ST_SF_SKIP Jump    Exit=PS_ST_SF_NUL
ITO PS_ST_SF_SF   Move   El1=RA_ALLOC_RESULT  Exit=PR_ST_FIRST
/NUL terminator word
CLEAR PS_ST_SF_NUL RA_TMP
RVOCA PS_ST_NUL_ALLOC ALLOC_LUX
ITO PS_ST_NUL_SET Write El1=RA_ALLOC_RESULT El2=C_0
/if PR_ST_FIRST still 0 (empty string), first = NUL Lux
JZ PS_ST_FIRSTCK PR_ST_FIRST PS_ST_EMPTY
ITO PS_ST_NOTEMPTY Jump   Exit=PS_ST_STORE
ITO PS_ST_EMPTY   Move   El1=RA_ALLOC_RESULT  Exit=PR_ST_FIRST
/store first Lux ID into the named Lux
ITO PS_ST_STORE   Write El1=PR_EL1 El2=PR_ST_FIRST
RREDI PS_ST_STRDONE

============================================================
/PS_CMD_SETREF — SETREF target source: store source's Lux ID
============================================================
RVOCA PS_CMD_SETREF PS_LK
ITO RET_AR_LK1_MV Move El1=PR_SYM     Exit=PR_EL1
/read + lookup second token via PS_NEXT_SYMBOL/
RVOCA CG_AR_NS  PS_NEXT_SYMBOL
ITO PS_AR_ST   Write El1=PR_EL1   El2=PR_SYM
RREDI PS_AR_RJ

============================================================
/PS_CMD_LINK — LINK src rel exit
============================================================
RVOCA PS_CMD_LINK PS_LK
ITO RET_LN_LK1_MV Move El1=PR_SYM     Exit=PR_EL1
/read + lookup rel token via PS_NEXT_SYMBOL
RVOCA CG_LN_NS2  PS_NEXT_SYMBOL
ITO RET_LN_NS2 Move    El1=PR_SYM     Exit=PR_EL2
/read + lookup exit_lux token via PS_NEXT_SYMBOL
RVOCA CG_LN_NS3  PS_NEXT_SYMBOL
ITO RET_LN_NS3 Move    El1=PR_SYM     Exit=PR_EL3
LINK_OP PS_LN_LK PR_EL1 PR_EL2 PR_EL3
RREDI PS_LN_RJ

============================================================
//PS_CMD_INSTR — ITO name op [key=val ...]
── ITO (INSTR) command ───────────────────────────────────────
Allocates a 7-lux block via ALLOC_LUCES(ITO_SIZE) and writes slots via
fixed offsets (no lumen scanning needed — layout is directly indexed).

ITO layout:  slot0=word(self-ref)  slot1=op  slot2=e1  slot3=e2
slot4=exit  slot5=next  slot6=pad  slot7+=extra_lumens

Key=val slot resolution: PS_LK_SLOT compares PR_SYM (key_lux addr) against
the 4 known relation lux addresses (Op,El1,El2,Exit) and returns
the slot index in PR_EL2. Exactly 6 valid keys, explicitly enumerated.
============================================================
read name token, look up//
RVOCA PS_CMD_INSTR PS_LK
JZ RET_IN_LK1_EZ PR_SYM PS_IN_NEW
ITO PS_IN_GOT  Move    El1=PR_SYM     Exit=PR_EL1   /found → use existing
ITO PS_IN_OP   Jump    Exit=PS_IN_OP_START
/create new ITO lux; self-ref + LSYM insert via PS_ALLOC_NAMED_ITO
RVOCA PS_IN_NEW PS_ALLOC_NAMED_ITO
/read op token + look up via PS_NEXT_SYMBOL
RVOCA PS_IN_OP_START PS_NEXT_SYMBOL
/write op_id into slot 1: aether[instr + SLOT_OP] = op_id
ITO PS_IN_WOP_A Add     El1=PR_EL1 El2=C_1 Exit=RA_TMP
ITO PS_IN_WOP   Write El1=RA_TMP  El2=PR_SYM
/key-value loop: read key token
RVOCA CG_IN_KVN  PS_NT
JZ RET_IN_KVN_EZ PR_TLEN PS_IN_DONE
/check for comment "//"
ITO PS_IN_KVCMT Read El1=PR_TBUF  Exit=PR_TMP
JEQ PS_IN_KVCM2 PR_TMP SLASH PS_IN_DONE
//look up key (relation name) — token already read by CG_IN_KVN
PS_LK returns addr(key_lux). PS_LK_SLOT maps it to the slot index
by comparing against the 6 known relation lux addresses: Op, El1, El2,
No graph relation needed — the 4 valid keys are fixed.//
RVOCA CG_IN_KVLK  PS_LK
/PR_SYM = key_lux addr → resolve slot index into PR_EL2 via PS_LK_SLOT
RVOCA RET_IN_KVLK_SL PS_LK_SLOT
/skip '='
ITO PS_IN_SKEQ Add     El1=PR_LPOS    El2=C_1  Exit=PR_LPOS
/read value token + look up via PS_NEXT_SYMBOL
RVOCA CG_IN_NSV  PS_NEXT_SYMBOL
/write val into slot: aether[instr + slot] = val_addr
ITO PS_IN_SLOTADDR Add     El1=PR_EL1 El2=PR_EL2 Exit=RA_TMP
ITO PS_IN_KVLNK    Write El1=RA_TMP El2=PR_SYM
ITO PS_IN_KVLB  Jump   Exit=CG_IN_KVN  /loop back for next key=val
CLEAR PS_IN_DONE RA_TMP
RREDI PS_IN_RJ

============================================================
//PS_LK_SLOT — map key_lux addr → slot index
IN:  PR_SYM = lux address of a relation name (result of PS_LK)
OUT: PR_EL2 = slot index in ITO compact layout, or 0 if unknown
Leaf. Exactly 4 valid ITO keys: Op El1 El2 Exit.
Slots: Op=1(SLOT_OP) El1=2(SLOT_E1) El2=3(SLOT_E2) Exit=4(SLOT_EXIT)
Comparing identity (addr) not word — SETREF invariant is preserved.//
============================================================
NOLINK
SWITCH PR_SYM
    Op   PS_LKS_OP
    El1  PS_LKS_A1
    El2  PS_LKS_A2
    Exit PS_LKS_EXIT
/Unknown key: return 0
CLEAR PS_LKS_UNK PR_EL2
RREDI PS_LKS_UNKR
NOITO
    PS_LKS_OP   Move El1=C_1        Exit=PR_EL2
        RREDI PS_LKS_OPR
    PS_LKS_A1   Move El1=SLOT_E1   Exit=PR_EL2
        RREDI PS_LKS_A1R
    PS_LKS_A2   Move El1=SLOT_E2   Exit=PR_EL2
        RREDI PS_LKS_A2R
    PS_LKS_EXIT Move El1=SLOT_EXIT Exit=PR_EL2
        RREDI PS_LKS_EXITR

============================================================
/PS_CMD_LOAD — LOAD filename: open file, parse it, close
============================================================
ITO PS_CMD_LOAD Move    El1=PR_READFD  Exit=PR_SAVED_READFD
ITO PS_CL_SEOF  Move    El1=PR_EOF     Exit=PR_SAVED_EOF
ITO PS_CL_X0    Move    El1=AT_FDCWD    Exit=SC_A0
ITO PS_CL_X1    Move    El1=PR_TBUF    Exit=SC_A1
ITO PS_CL_X2    Move    El1=O_RDONLY    Exit=SC_A2
CLEAR PS_CL_X3 SC_A3
ITO PS_CL_X8    Move    El1=SYS_OPENAT Exit=SC_NR
ITO PS_CL_SC    Exire
ITO PS_CL_FD    Move    El1=SC_A0      Exit=PR_LOAD_FD
ITO PS_CL_ERR   Less El1=PR_LOAD_FD El2=C_0 Exit=PR_TMP
ITO PS_CL_ERRJ  JumpIf  El1=PR_TMP    Exit=PS_CL_RESTORE  /error → restore
ITO PS_CL_SETFD Move    El1=PR_LOAD_FD Exit=PR_READFD
CLEAR PS_CL_SEOF2 PR_EOF
CLEAR PS_CL_LPINIT PR_LPOS
/inner loop: parse lines from the loaded file
RVOCA CG_CL_INNER  PS_LINE
JEQ RET_CL_INNER_EOF PR_EOF C_1 PS_CL_CLOSE
ITO PS_CL_LOOP  Jump    Exit=CG_CL_INNER  /loop back
ITO PS_CL_CLOSE Move    El1=PR_LOAD_FD   Exit=SC_A0
ITO PS_CL_CX8   Move    El1=SYS_CLOSE    Exit=SC_NR
ITO PS_CL_CSC   Exire
ITO PS_CL_RESTORE Move  El1=PR_SAVED_READFD Exit=PR_READFD
ITO PS_CL_REOF  Move    El1=PR_SAVED_EOF    Exit=PR_EOF
RREDI PS_CL_RJ

============================================================
//PS_LINE — parse and dispatch one line from LBUF
Non-leaf. Returns via Redi (RA_LINK).//
/============================================================
/read line
RVOCA PS_LINE    PS_RL
JZ RET_PL_RL_BLK PR_LLEN PS_LINE_DONE
/read first token (the keyword)/
RVOCA CG_PL_NT    PS_NT
JZ RET_PL_NT_EMP PR_TLEN PS_LINE_DONE
/dispatch via BS_LOOKUP: PR_HASH already computed by PS_NT
ITO PS_LINE_KLK  Move El1=BS_HT_BASE   Exit=RA_HT_BASE
ITO PS_LINE_KHS  Move El1=PR_HASH      Exit=RA_HT_HASH
ITO PS_LINE_KSZ  Move El1=BS_HT_SIZE   Exit=RA_HT_SIZE
ITO PS_LINE_KMK  Move El1=BS_HT_MASK   Exit=RA_HT_MASK
RVOCA PS_LINE_KHT HT_LOOKUP
JZ PS_LINE_KNF RA_HT_RESULT PS_LINE_DONE
ITO PS_LINE_KRD  Read El1=RA_HT_RESULT Exit=RA_TMP
JZ PS_LINE_KHZ RA_TMP PS_LINE_DONE
ITO PS_LINE_KDJ  JumpReg El1=RA_TMP
CLEAR PS_LINE_DONE RA_TMP
RREDI PS_LINE_RJ

── NEW command ───────────────────────────────────────────────
NOLINK
RVOCA PS_LINE_NEW PS_NT
RVOCA CG_PL_NEW  PS_CMD_NEW
ITO PS_LINE_NWD Jump    Exit=PS_LINE_DONE

── NEWSET command: NEWSET name value ─────────────────────────
//Matryoshka: NEW name + SET name value in one command.
Reads name via PS_NT+PS_NEXT_SYMBOL, value via PS_NT+PS_CMD_SET path.//
NOLINK
RVOCA PS_LINE_NEWSET PS_NT
RVOCA CG_PL_NEWSET_NS PS_CMD_NEW
RVOCA CG_PL_NEWSET_VAL PS_NT
RVOCA CG_PL_NEWSET_SET PS_CMD_SET
ITO PS_LINE_NEWSET_DONE Jump Exit=PS_LINE_DONE

── NEXO command: NEXO aspect body ────────────────────────────
//Appends one RO-lux body line to Yaku_aspect (which must already exist).
Body is rest of line after aspect name (no quotes).//
NOLINK
RVOCA PS_LINE_NEXO PS_NEXT_SYMBOL
ITO PS_NEXO_SAV  Move El1=PR_SYM Exit=PR_MAC_ARG1
//PR_MAC_ARG1 = aspect Lux ID. Look up Yaku_aspect in LSYM.
For now: build the body graph and append to Yaku_aspect's chain.//
ITO PS_NEXO_MKRN Move El1=PR_MAC_ARG1 Exit=PR_YAKU_RULE_LUX
RCALL_AT PS_NEXO_BGRET PS_DEF_BUILD_BODY PS_NEXO_DONE
ITO PS_NEXO_DONE Jump Exit=PS_LINE_DONE

── NOLINK command ────────────────────────────────────────────
NOLINK
CLEAR PS_LINE_NOLINK PR_LAST_INSTR
ITO PS_NOLINK_DONE Jump   Exit=PS_LINE_DONE

── NOP command: NOP name → ITO name Move El1=C_0 Exit=C_0 ──
/Reads name token, creates ITO lux, wires Op=Move, El1=C_0, Exit=C_0.
RVOCA PS_LINE_NOP  PS_NEXT_SYMBOL
/PR_SYM → PR_EL1 (find or alloc ITO lux)
RVOCA PS_NOP_MOF PS_MAKE_OR_FIND_ITO
/Wire: Op=Move at slot 1, El1=C_0 at SLOT_E1, Exit=C_0 at SLOT_EXIT
CLEAR PS_NOP_WIRE RA_TMP
ITO PS_NOP_LOP_A  Add    El1=PR_EL1 El2=C_1 Exit=RA_TMP   /slot 1 = SLOT_OP
ITO PS_NOP_LOP   Write El1=RA_TMP El2=Move
ITO PS_NOP_LA1_A  Add     El1=PR_EL1 El2=SLOT_E1 Exit=RA_TMP
ITO PS_NOP_LA1    Write El1=RA_TMP El2=C_0
ITO PS_NOP_LTG_A  Add     El1=PR_EL1 El2=SLOT_EXIT Exit=RA_TMP
ITO PS_NOP_LTG    Write El1=RA_TMP El2=C_0
ITO PS_NOP_DONE Jump Exit=PS_SINGLE_ITO_DONE

── BLOCK command ─────────────────────────────────────────────
//BLOCK name count → allocate count consecutive Lux, insert first into LSYM.
Reads: name (via PS_NEXT_SYMBOL), count (via PS_NT + PS_INT).
Creates count Lux via ALLOC_LUX loop.
Inserts first Lux into LSYM via PS_INS (hash=PR_HASH from name token).//
NEW PR_BLK_FIRST    /first Lux ID of BLOCK/
NEW PR_BLK_CNT      /remaining count/
NEW PR_BLK_HASH     /saved hash of name token/

RVOCA PS_LINE_BLK  PS_NEXT_SYMBOL
/save hash of name token (PR_HASH was set by PS_NT inside PS_NEXT_SYMBOL)
ITO PS_BLK_SAVEH Move   El1=PR_HASH   Exit=PR_BLK_HASH
/read count token
RVOCA CG_PL_BLK_CNT_NT PS_NT
/parse count as integer
RVOCA CG_PL_BLK_CNT_PI  PS_INT
/PR_VAL = count. Allocate first Lux.
ITO PS_BLK_SAVECNT Move  El1=PR_VAL   Exit=PR_BLK_CNT
RVOCA PS_BLK_ALLOC1 ALLOC_LUX
ITO PS_BLK_SAVFST Move   El1=RA_ALLOC_RESULT Exit=PR_BLK_FIRST
/insert first Lux into LSYM with saved hash
ITO PS_BLK_SETHASH Move  El1=PR_BLK_HASH Exit=PR_HASH
ITO PS_BLK_SETARG1 Move  El1=PR_BLK_FIRST Exit=PR_EL1
RVOCA CG_BLK_INS PS_INS
/allocate remaining count-1 Lux (no symbol insertion, just consecutive allocation)
ITO PS_BLK_DEC   Sub     El1=PR_BLK_CNT El2=C_1 Exit=PR_BLK_CNT
JZ PS_BLK_LOOP PR_BLK_CNT PS_LINE_DONE
/allocate one more Lux (ID discarded — they are consecutive by Aether allocator)
RVOCA PS_BLK_ALLOCN ALLOC_LUX
ITO PS_BLK_DEC2  Sub     El1=PR_BLK_CNT El2=C_1 Exit=PR_BLK_CNT
ITO PS_BLK_LB    Jump    Exit=PS_BLK_LOOP

── SET command ───────────────────────────────────────────────
NOLINK
RVOCA PS_LINE_SET PS_NT
RVOCA CG_PL_SET  PS_CMD_SET
ITO PS_LINE_STD Jump    Exit=PS_LINE_DONE

── SETREF command ────────────────────────────────────────────
NOLINK
RVOCA PS_LINE_SRF PS_NT
RVOCA CG_PL_SR  PS_CMD_SETREF
ITO PS_LINE_SRD2 Jump   Exit=PS_LINE_DONE

── LINK command ──────────────────────────────────────────────
NOLINK
RVOCA PS_LINE_LNK PS_NT
RVOCA CG_PL_LNK  PS_CMD_LINK
ITO PS_LINE_LND Jump    Exit=PS_LINE_DONE

── LOAD command ──────────────────────────────────────────────
NOLINK
RVOCA PS_LINE_LOAD PS_NT
RVOCA CG_PL_LOAD  PS_CMD_LOAD
ITO PS_LINE_LDD Jump    Exit=PS_LINE_DONE

── LINK_OP command ───────────────────────────────────────────
/Falls through to PS_LINE_LNKOP (defined below)

── ITO command ────────────────────────────────────────────
RVOCA PS_LINE_INS PS_NT
RVOCA CG_PL_INS  PS_CMD_INSTR
ITO PS_LINE_IND Jump Exit=PS_SINGLE_ITO_DONE


── RVOCA: name SUB → ITO name Voca El1=SUB ───────────────
RVOCA PS_LINE_RCALL  PS_NEXT_SYMBOL
/PR_SYM → PR_EL1 (find or alloc ITO lux)
RVOCA PS_RCALL_MOF PS_MAKE_OR_FIND_ITO
/write Op=Voca at slot 1 (SLOT_OP)
ITO PS_RCALL_WIRE_OP   Add El1=PR_EL1 El2=C_1 Exit=RA_TMP
ITO PS_RCALL_WOP     Write El1=RA_TMP El2=Voca
/read SUB token
RVOCA CG_PL_RCALL_SB  PS_NEXT_SYMBOL
/write El1=SUB at SLOT_E1
ITO PS_RCALL_LA1_A  Add     El1=PR_EL1 El2=SLOT_E1 Exit=RA_TMP
ITO PS_RCALL_LA1    Write El1=RA_TMP El2=PR_SYM
/write Exit=RA_LINK at SLOT_EXIT (Voca saves nxt there)
ITO PS_RCALL_LXT_A  Add     El1=PR_EL1 El2=SLOT_EXIT Exit=RA_TMP
ITO PS_RCALL_LXT    Write El1=RA_TMP El2=RA_LINK
ITO PS_RCALL_DONE Jump Exit=PS_SINGLE_ITO_DONE

── RREDI: name → ITO name Redi ──────────────────────────────
RVOCA PS_LINE_RRET  PS_NEXT_SYMBOL
/PR_SYM → PR_EL1 (find or alloc ITO lux)
RVOCA PS_RRET_MOF PS_MAKE_OR_FIND_ITO
/write Op=Redi at slot 1 (SLOT_OP)
ITO PS_RRET_WIRE_OP Add  El1=PR_EL1 El2=C_1 Exit=RA_TMP
ITO PS_RRET_LOP  Write El1=RA_TMP El2=Redi
/write El1=RA_LINK at SLOT_E1 (Redi jumps to aether[El1])
ITO PS_RRET_LA1_A  Add   El1=PR_EL1 El2=SLOT_E1 Exit=RA_TMP
ITO PS_RRET_LA1    Write El1=RA_TMP El2=RA_LINK
ITO PS_RRET_DONE Jump Exit=PS_SINGLE_ITO_DONE

============================================================
/PS_MAIN — top-level loop: reads and parses lines until EOF
============================================================
ITO PS_MAIN    Read  El1=PS_LBUF_BASE Exit=PR_LBUF
ITO PS_MAIN_TB Read  El1=PS_TBUF_BASE Exit=PR_TBUF
ITO PS_MAIN_PB Read  El1=PS_PBKT_BASE Exit=PR_PBKT
ITO PS_MAIN_LS Read  El1=PS_LSYM_BASE Exit=PR_LSYM
CLEAR PS_MAIN_EI PR_EOF
CLEAR PS_MAIN_FD PR_READFD
CLEAR PS_MAIN_LP PR_LPOS
/main loop
JEQ PS_MAIN_H PR_EOF C_1 PS_MAIN_END
RVOCA CG_MAIN_PL  PS_LINE
ITO RET_MAIN_PL_LB Jump Exit=PS_MAIN_H    /loop back/
ITO PS_MAIN_END End

============================================================
//PS_LK_PREFIX3 — shared handler for "XB_NNN" → BASE + NNN lookups
Handles OB_NNN and DS_NNN patterns.

IN:  PR_PFX_CHAR = expected char at tbuf[1] ('B' or 'S')
PR_PFX_BASE = base value (word of RA_OB_BASE or RA_DS_BASE)
OUT: PR_SYM = BASE + NNN on hit; PR_SYM = 0 on miss
Leaf. Returns via Redi (RA_LINK).//
============================================================
NOLINK
ITO PS_LK_PREFIX3  Add    El1=PR_TBUF  El2=C_1    Exit=PR_TMP
ITO PFX3_R1        Read El1=PR_TMP              Exit=PR_TMP
JEQ PFX3_CMP1 PR_TMP PR_PFX_CHAR PFX3_CHK2
CLEAR PFX3_MISS PR_SYM
RREDI PFX3_MISSJ
ITO PFX3_CHK2      Add    El1=PR_TBUF  El2=C_2    Exit=PR_TMP
ITO PFX3_R2        Read El1=PR_TMP              Exit=PR_TMP
JEQ PFX3_CMP2 PR_TMP UNDERSCORE PFX3_D3
CLEAR PFX3_MISS2 PR_SYM
RREDI PFX3_MISS2J
ITO PFX3_D3        Move    El1=C_3    Exit=PR_D3_OFF
RCALL_AT PFX3_D3R    PS_D3  PFX3_DONE
ITO PFX3_DONE      Add    El1=PR_PFX_BASE El2=PR_D3_VAL Exit=PR_SYM
RREDI PFX3_RETJ

============================================================
//PS_NEXT_SYMBOL — read next token then look up its symbol
Combines PS_NT + PS_LK. Non-leaf (OUTER save). Voca with RVOCA.
OUT: PR_SYM = found Lux ID (0 if not found)//
============================================================
NOLINK
RVOCA PS_NEXT_SYMBOL  PS_NT
RVOCA PNS_LKJ  PS_LK
RREDI PNS_RET_r

── PS_READ_NAME_MOF — PS_NEXT_SYMBOL + PS_MAKE_OR_FIND + save ─
//Reads next token, looks it up or creates it, saves ID to PR_MAC_NAME.
Used at start of every macro handler (16 times). Non-leaf.//
NOLINK
RVOCA PS_READ_NAME_MOF   PS_NEXT_SYMBOL
RVOCA RNM_MOF  PS_MAKE_OR_FIND
ITO RNM_SAV  Move El1=PR_EL1           Exit=PR_MAC_NAME
RREDI RNM_RET_r

── Shared registers for new macro handlers ────────────────────
NEW PR_MAC_NAME      /Lux ID of macro's primary name Lux
NEW PR_MAC_ARG1      /first element Lux ID
NEW PR_MAC_ARG2      /second element Lux ID
NEW PR_MAC_ARG3      /third element Lux ID
NEW PR_MAC_TMP       /scratch for macro builders
NEW PR_MAC_NM_K      /name_K Lux (NOP continuation for EMIT/CALL macros)
NEW PR_MAC_NM_J      /name_J Lux
NEW PR_MAC_NM_R      /name_R Lux

── PS_MK_EMIT_PATTERN parameters ─────────────────────────────
//Used by EMIT / EMITI / PUTBYTE handlers — all share the same
4-Lux structure (name, name_R, name_J, name_K). Only the
destination subroutine and the target register differ.//
NEW PR_EMP_DEST      /IN: Lux ID of the destination subroutine (EMIT_STR_ENTRY etc.)
NEW PR_JCQ_B2        /IN: second operand for PS_MK_JCQ_PATTERN/
NEW PR_CAL_LANDING   /IN: landing Lux ID for PS_MK_RCALL_PATTERN/
/PR_JT_BASE_REG/DEF_REG/PSI_OUTER removed — PS_INIT_JT128 now uses RA_TMP2/RA_TMP3 directly (leaf)
NEW PR_LOP_LUMENOP   /IN: ADD_LUMEN or REMOVE_LUMEN for PS_MK_LOP_PATTERN
NEW PR_GL_SRFUNC     /IN: SR_GLL/SR_GLR/SR_GLE/SR_GLX for PS_MK_GL_PATTERN
NEW PR_GL_NAMETGT    /IN: RA_SR_LUX or RA_SR_LUMEN for PS_MK_GL_PATTERN
NEW PR_EMP_TGT       /IN: Lux ID of the target register (RA_TW_LUX / RA_TMP2 / RA_BYTE)


── PS_SINGLE_ITO_DONE: shared tail for single-lux commands (NOP/RVOCA/RREDI/ITO) ─
//IN: PR_EL1 = the single instruction lux created.
Sets PR_AN_FIRST = PR_AN_LAST = PR_EL1, then falls into PS_INSTR_DONE.//
NOLINK
ITO PS_SINGLE_ITO_DONE Move El1=PR_EL1 Exit=PR_AN_FIRST
ITO PSID_LAST          Move El1=PR_EL1 Exit=PR_AN_LAST
ITO PSID_JMP           Jump Exit=PS_INSTR_DONE

── PS_INSTR_DONE: universal exit for instruction-creating commands ─
//Wires prev[SLOT_NEXT=5] = PR_AN_FIRST to auto-link sequential instructions.
IN:  PR_AN_FIRST = first Lux of current sequence (gets incoming nxt link)
PR_AN_LAST  = last Lux of current sequence (stored as PR_LAST_INSTR)
Flow:
if PR_LAST_INSTR == 0: skip link, just update tracker
if PR_LAST_INSTR != 0: write aether[prev + SLOT_NEXT] = PR_AN_FIRST, then update//
NOLINK
JZ    PS_INSTR_DONE  PR_LAST_INSTR              PID_DO_UPDATE
/PR_LAST_INSTR != 0: write next-link into slot 5 of previous instruction
ITO PID_LINK_A     Add     El1=PR_LAST_INSTR El2=C_5    Exit=RA_TMP   /addr+SLOT_NEXT
ITO PID_LINK_B     Write El1=RA_TMP         El2=PR_AN_FIRST           /prev[5] = first
NOLINK
/Update tracker: PR_LAST_INSTR = PR_AN_LAST (last lux of this sequence)
ITO PID_DO_UPDATE  Move    El1=PR_AN_LAST    Exit=PR_LAST_INSTR
RREDI PID_RET_r

── PS_MAKE_OR_FIND_ITO: find existing ITO lux or alloc new named ITO ──────
//IN:  PR_SYM = existing lux addr (0 → create ITO via PS_ALLOC_NAMED_ITO)
        PR_HASH = token hash (used if creating)
OUT: PR_EL1 = lux addr to use (existing or newly allocated ITO).
Leaf when found; non-leaf (calls PS_ALLOC_NAMED_ITO) when creating.//
NOLINK
JZ PS_MAKE_OR_FIND_ITO PR_SYM MOFI_NEW
ITO MOFI_GOT  Move El1=PR_SYM Exit=PR_EL1
RREDI MOFI_GOT_r
RVOCA MOFI_NEW PS_ALLOC_NAMED_ITO      /alloc + self-ref + LSYM insert
RREDI MOFI_NEWRET_r

── PS_MAKE_OR_FIND: look up PR_SYM or create Lux, insert into LSYM ─
//IN:  PR_SYM = 0 → create and insert; nonzero → use existing
IN:  PR_HASH = hash of token for insertion
OUT: PR_EL1 = Lux ID to use//
NOLINK
JZ PS_MAKE_OR_FIND PR_SYM PS_MOF_NEW
ITO PS_MOF_GOT      Move   El1=PR_SYM    Exit=PR_EL1
RREDI PS_MOF_GOTR_r
RVOCA PS_MOF_NEW ALLOC_LUX
ITO PS_MOF_SETPRE   Move   El1=RA_ALLOC_RESULT Exit=PR_EL1
RVOCA CG_MOF_INS      PS_INS
RREDI PS_MOF_NEWR_r

── PS_ALLOC_NAMED_ITO: alloc ITO lux, self-ref, insert into LSYM ─────────
//Named alloc: used when the ITO is the primary named lux (ITO cmd, NOP, RVOCA, RREDI).
IN:  PR_HASH = token hash for LSYM insertion.
OUT: PR_EL1 = new ITO lux address (also inserted into LSYM).
Non-leaf (calls PS_INS); RA_LINK saved/restored automatically.//
NOLINK
ALLOC_TO PS_ALLOC_NAMED_ITO PR_EL1 ITO_SIZE
ITO ANI_SELF  Write El1=PR_EL1 El2=PR_EL1
RVOCA ANI_INS PS_INS
RREDI ANI_RET_r

── PS_ALLOC_SUBLUX: alloc anonymous ITO lux (no LSYM insert) ─────────────
//Used for sub-luces built by macro patterns (name_J, name_K, etc.).
OUT: PR_EL1 = new ITO lux address.
Non-leaf — RA_LINK is saved/restored automatically by the call stack.//
NOLINK
ALLOC_TO PS_ALLOC_SUBLUX PR_EL1 ITO_SIZE
ITO PS_ASL_WSELF     Write El1=PR_EL1 El2=PR_EL1
RREDI PS_ASL_RET_r

── PS_MK_RCALL_PATTERN — name(Move landing→RA_LINK) → name_J(Jump sub) ─
//IN:  PR_MAC_NAME, PR_MAC_ARG1(sub dest), PR_CAL_LANDING(landing ID)
Non-leaf — RA_LINK is saved/restored automatically by the call stack.//
NOLINK
RVOCA PS_MK_RCALL_PATTERN PS_ALLOC_SUBLUX
ITO CAL_SAVJ Move El1=PR_EL1   Exit=PR_MAC_NM_J
ITO CAL_LJO_A  Add     El1=PR_MAC_NM_J El2=C_1 Exit=RA_TMP2
ITO CAL_LJO  Write El1=RA_TMP2 El2=Jump
ITO CAL_LJD_A  Add     El1=PR_MAC_NM_J El2=SLOT_EXIT Exit=RA_TMP
ITO CAL_LJD    Write El1=RA_TMP El2=PR_MAC_ARG1
ITO CAL_LNO_A  Add     El1=PR_MAC_NAME El2=C_1 Exit=RA_TMP2
ITO CAL_LNO  Write El1=RA_TMP2 El2=Move
ITO CAL_LNA_A  Add     El1=PR_MAC_NAME El2=SLOT_E1 Exit=RA_TMP
ITO CAL_LNA    Write El1=RA_TMP El2=PR_CAL_LANDING
ITO CAL_LNT_A  Add     El1=PR_MAC_NAME El2=SLOT_EXIT Exit=RA_TMP
ITO CAL_LNT    Write El1=RA_TMP El2=RA_LINK
RREDI CAL_RET_r

── EMIT / EMITI / PUTBYTE — unified via PS_MK_EMIT_PATTERN ──
//All three create the same 4-Lux structure:
name: Move arg → target_reg
name_R: Move name_K → RA_LINK
name_J: Jump → dest_sub
name_K: Move C_0 → C_0  (NOP continuation)
IN (set before RVOCA PS_MK_EMIT_PATTERN):
PR_EMP_DEST = destination subroutine Lux ID
PR_EMP_TGT  = target register Lux ID
PR_MAC_NAME / PR_MAC_ARG1 already set by caller//
── Dispatch: tbuf[4]=='I' → EMITI, else EMIT ────────────────
── EMIT command ──────────────────────────────────────────────
NOLINK
/EMIT: name + arg
RVOCA CG_PL_EMIT_NM PS_READ_NAME_MOF
RVOCA CG_PL_EMIT_A  PS_NEXT_SYMBOL
ITO PS_EMIT_SAVA  Move  El1=PR_SYM    Exit=PR_MAC_ARG1
ITO PS_EMIT_SDST  Move  El1=EMIT_STR_ENTRY Exit=PR_EMP_DEST
ITO PS_EMIT_STGT  Move  El1=RA_TW_LUX     Exit=PR_EMP_TGT
RVOCA PS_EMIT_PAT   PS_MK_EMIT_PATTERN
ITO PS_EMIT_DONE3 Jump  Exit=PS_INSTR_DONE

/EMITI: name + value_reg
RVOCA PS_LINE_EMITI PS_READ_NAME_MOF
RVOCA CG_PL_EMITI_A  PS_NEXT_SYMBOL
ITO PS_EMITI_SAVA  Move El1=PR_SYM    Exit=PR_MAC_ARG1
ITO PS_EMITI_SDST  Move El1=EMIT_INT_ENTRY Exit=PR_EMP_DEST
ITO PS_EMITI_STGT  Move El1=RA_TMP2       Exit=PR_EMP_TGT
RVOCA PS_EMITI_PAT   PS_MK_EMIT_PATTERN
ITO PS_EMITI_DONE3 Jump Exit=PS_INSTR_DONE

/PUTBYTE: name + byte_val
RVOCA PS_LINE_PUTB PS_READ_NAME_MOF
RVOCA CG_PL_PUTB_A  PS_NEXT_SYMBOL
ITO PS_PUTB_SAVA  Move  El1=PR_SYM     Exit=PR_MAC_ARG1
ITO PS_PUTB_SDST  Move  El1=PUT_BYTE       Exit=PR_EMP_DEST
ITO PS_PUTB_STGT  Move  El1=RA_BYTE        Exit=PR_EMP_TGT
RVOCA PS_PUTB_PAT   PS_MK_EMIT_PATTERN
ITO PS_PUTB_DONE3 Jump  Exit=PS_INSTR_DONE

── PS_MK_EMIT_PATTERN — build 3-Lux emit sequence ────────────
//IN:  PR_MAC_NAME = name Lux, PR_MAC_ARG1 = arg Lux
PR_EMP_DEST = Voca target, PR_EMP_TGT = Move target reg
OUT: PR_AN_FIRST = PR_MAC_NAME, PR_AN_LAST = PR_MAC_NM_K
Structure: name(Move arg→reg) → name_J(Voca dest Exit=RA_LINK) → name_K(NOP)
Non-leaf — RA_LINK is saved/restored automatically by the call stack.//
NOLINK
/name_K: Move C_0 → C_0  (NOP landing — Voca saves nxt=name_K into RA_LINK)
RVOCA PS_MK_EMIT_PATTERN PS_ALLOC_SUBLUX
ITO EMP_SAVK Move El1=PR_EL1 Exit=PR_MAC_NM_K
ITO EMP_LKO_A  Add     El1=PR_MAC_NM_K El2=C_1 Exit=RA_TMP2
ITO EMP_LKO  Write El1=RA_TMP2 El2=Move
ITO EMP_LKA_A  Add     El1=PR_MAC_NM_K El2=SLOT_E1 Exit=RA_TMP
ITO EMP_LKA    Write El1=RA_TMP El2=C_0
ITO EMP_LKT_A  Add     El1=PR_MAC_NM_K El2=SLOT_EXIT Exit=RA_TMP
ITO EMP_LKT    Write El1=RA_TMP El2=C_0
/name_J: Voca El1=dest Exit=RA_LINK (saves nxt into RA_LINK, jumps to dest)
RVOCA EMP_MKJ PS_ALLOC_SUBLUX
ITO EMP_SAVJ Move El1=PR_EL1 Exit=PR_MAC_NM_J
ITO EMP_LJO_A  Add     El1=PR_MAC_NM_J El2=C_1 Exit=RA_TMP2
ITO EMP_LJO  Write El1=RA_TMP2 El2=Voca
ITO EMP_LJA_A  Add     El1=PR_MAC_NM_J El2=SLOT_E1 Exit=RA_TMP
ITO EMP_LJA    Write El1=RA_TMP El2=PR_EMP_DEST
ITO EMP_LJX_A  Add     El1=PR_MAC_NM_J El2=SLOT_EXIT Exit=RA_TMP
ITO EMP_LJX    Write El1=RA_TMP El2=RA_LINK
/name: Move arg → PR_EMP_TGT
ITO EMP_LNO_A  Add     El1=PR_MAC_NAME El2=C_1 Exit=RA_TMP2
ITO EMP_LNO  Write El1=RA_TMP2 El2=Move
ITO EMP_LNA_A  Add     El1=PR_MAC_NAME El2=SLOT_E1 Exit=RA_TMP
ITO EMP_LNA    Write El1=RA_TMP El2=PR_MAC_ARG1
ITO EMP_LNT_A  Add     El1=PR_MAC_NAME El2=SLOT_EXIT Exit=RA_TMP
ITO EMP_LNT    Write El1=RA_TMP El2=PR_EMP_TGT
/chain: name→name_J→name_K
ITO EMP_FIRST Move El1=PR_MAC_NAME Exit=PR_AN_FIRST
ITO EMP_LAST  Move El1=PR_MAC_NM_K Exit=PR_AN_LAST
RREDI EMP_RET_r

── JEQ macro: JEQ name A B dest ─────────────────────────────
//name:   Equal El1=A El2=B Exit=RA_FLAG
name_J: JumpIf El1=RA_FLAG Exit=dest
tbuf[1]: 'E'→JEQ, 'Z'→JZ//
── JEQ command ───────────────────────────────────────────────
NOLINK
/JEQ: name A B dest — Equal A B then JumpIf
RVOCA CG_PL_JEQ_NM PS_READ_NAME_MOF
RVOCA CG_PL_JEQ_A  PS_NEXT_SYMBOL
ITO PS_JEQ_SAVA  Move El1=PR_SYM   Exit=PR_MAC_ARG1
RVOCA CG_PL_JEQ_B  PS_NEXT_SYMBOL
ITO PS_JEQ_SAVB  Move El1=PR_SYM   Exit=PR_MAC_ARG2
RVOCA CG_PL_JEQ_D  PS_NEXT_SYMBOL
ITO PS_JEQ_SAVD  Move El1=PR_SYM   Exit=PR_MAC_ARG3
ITO PS_JEQ_SB2   Move El1=PR_MAC_ARG2 Exit=PR_JCQ_B2
RVOCA PS_JEQ_PAT   PS_MK_JCQ_PATTERN
ITO PS_JEQ_DONE3 Jump Exit=PS_INSTR_DONE

/JZ: JZ name A dest — JZ is JEQ with B=C_0 (partial case)
RVOCA PS_LINE_JZ PS_READ_NAME_MOF
RVOCA CG_PL_JZ_A  PS_NEXT_SYMBOL
ITO PS_JZ_SAVA  Move El1=PR_SYM   Exit=PR_MAC_ARG1
RVOCA CG_PL_JZ_D  PS_NEXT_SYMBOL
ITO PS_JZ_SAVD  Move El1=PR_SYM   Exit=PR_MAC_ARG3
CLEAR PS_JZ_SB2 PR_JCQ_B2
RVOCA PS_JZ_PAT   PS_MK_JCQ_PATTERN
ITO PS_JZ_DONE3 Jump Exit=PS_INSTR_DONE

── PS_MK_JCQ_PATTERN — Equal + JumpIf structure ──────────
//IN:  PR_MAC_NAME, PR_MAC_ARG1(A), PR_JCQ_B2(B), PR_MAC_ARG3(dest)
Builds: name(Equal A B→RA_FLAG) → name_J(JumpIf RA_FLAG Exit=dest
Non-leaf — RA_LINK is saved/restored automatically by the call stack.//
NOLINK
/name_J: JumpIf RA_FLAG → dest
RVOCA PS_MK_JCQ_PATTERN PS_ALLOC_SUBLUX
ITO JCQ_SAVJ Move El1=PR_EL1 Exit=PR_MAC_NM_J
ITO JCQ_LJO_A  Add     El1=PR_MAC_NM_J El2=C_1 Exit=RA_TMP2
ITO JCQ_LJO  Write El1=RA_TMP2 El2=JumpIf
ITO JCQ_LJA_A  Add     El1=PR_MAC_NM_J El2=SLOT_E1 Exit=RA_TMP
ITO JCQ_LJA    Write El1=RA_TMP El2=RA_FLAG
ITO JCQ_LJD_A  Add     El1=PR_MAC_NM_J El2=SLOT_EXIT Exit=RA_TMP
ITO JCQ_LJD    Write El1=RA_TMP El2=PR_MAC_ARG3
/name: Equal A B → RA_FLAG
ITO JCQ_LNO_A  Add     El1=PR_MAC_NAME El2=C_1 Exit=RA_TMP2
ITO JCQ_LNO  Write El1=RA_TMP2 El2=Equal
ITO JCQ_LNA_A  Add     El1=PR_MAC_NAME El2=SLOT_E1 Exit=RA_TMP
ITO JCQ_LNA    Write El1=RA_TMP El2=PR_MAC_ARG1
ITO JCQ_LNB_A  Add     El1=PR_MAC_NAME El2=SLOT_E2 Exit=RA_TMP
ITO JCQ_LNB    Write El1=RA_TMP El2=PR_JCQ_B2
ITO JCQ_LNT_A  Add     El1=PR_MAC_NAME El2=SLOT_EXIT Exit=RA_TMP
ITO JCQ_LNT    Write El1=RA_TMP El2=RA_FLAG
ITO JCQ_FIRST Move El1=PR_MAC_NAME Exit=PR_AN_FIRST
ITO JCQ_LAST  Move El1=PR_MAC_NM_J Exit=PR_AN_LAST
RREDI JCQ_RET_r

── LINK_OP macro: LINK_OP name src rel exit ──────────────────
//name:   Move src → RA_LM_SRC
name_R: Move rel → RA_LM_REL
name_T: Move exit_lux → RA_LM_EXIT
name_J: RVOCA → Voca El1=ADD_LUMEN
── LINK_OP / UNLINK_OP — unified via PS_MK_LOP_PATTERN ──────
Both create: name(Move src→SRC) → name_R(Move rel→REL) → name_T(Move exit_lux→EXIT_N) → name_J(Voca lumen_op)
Only difference: ADD_LUMEN vs REMOVE_LUMEN.//
RVOCA PS_LINE_LNKOP PS_READ_NAME_MOF
RVOCA CG_PL_LNKOP_S  PS_NEXT_SYMBOL
ITO PS_LNKOP_SAVS  Move El1=PR_SYM    Exit=PR_MAC_ARG1
RVOCA CG_PL_LNKOP_R  PS_NEXT_SYMBOL
ITO PS_LNKOP_SAVR  Move El1=PR_SYM    Exit=PR_MAC_ARG2
RVOCA CG_PL_LNKOP_T  PS_NEXT_SYMBOL
ITO PS_LNKOP_SAVT  Move El1=PR_SYM    Exit=PR_MAC_ARG3
ITO PS_LNKOP_SLOP  Move El1=ADD_LUMEN Exit=PR_LOP_LUMENOP
RVOCA PS_LNKOP_PAT   PS_MK_LOP_PATTERN
ITO PS_LNKOP_DONE3 Jump Exit=PS_INSTR_DONE

/UNLINK_OP: same but REMOVE_LUMEN
RVOCA PS_LINE_ULOP PS_READ_NAME_MOF
RVOCA CG_PL_ULOP_S  PS_NEXT_SYMBOL
ITO PS_ULOP_SAVS  Move El1=PR_SYM       Exit=PR_MAC_ARG1
RVOCA CG_PL_ULOP_R  PS_NEXT_SYMBOL
ITO PS_ULOP_SAVR  Move El1=PR_SYM       Exit=PR_MAC_ARG2
RVOCA CG_PL_ULOP_T  PS_NEXT_SYMBOL
ITO PS_ULOP_SAVT  Move El1=PR_SYM       Exit=PR_MAC_ARG3
ITO PS_ULOP_SLOP  Move El1=REMOVE_LUMEN Exit=PR_LOP_LUMENOP
RVOCA PS_ULOP_PAT   PS_MK_LOP_PATTERN
ITO PS_ULOP_DONE3 Jump Exit=PS_INSTR_DONE

── PS_MK_LOP_PATTERN — build 4-Lux link/unlink sequence ──────
//IN:  PR_MAC_NAME, PR_MAC_ARG1(src), PR_MAC_ARG2(rel), PR_MAC_ARG3(tgt)
PR_LOP_LUMENOP = ADD_LUMEN or REMOVE_LUMEN
OUT: PR_AN_FIRST = PR_MAC_NAME, PR_AN_LAST = PR_MAC_NM_J
Non-leaf — RA_LINK is saved/restored automatically by the call stack.//
NOLINK
/name: Move src → RA_LM_SRC
ITO PS_MK_LOP_PATTERN  Add     El1=PR_MAC_NAME El2=C_1 Exit=RA_TMP2
ITO LOP_LNO  Write El1=RA_TMP2 El2=Move
ITO LOP_LNA_A  Add     El1=PR_MAC_NAME El2=SLOT_E1 Exit=RA_TMP
ITO LOP_LNA    Write El1=RA_TMP El2=PR_MAC_ARG1
ITO LOP_LNT_A  Add     El1=PR_MAC_NAME El2=SLOT_EXIT Exit=RA_TMP
ITO LOP_LNT    Write El1=RA_TMP El2=RA_LM_SRC
/name_R: Move rel → RA_LM_REL
RVOCA LOP_MKR PS_ALLOC_SUBLUX
ITO LOP_SAVR Move El1=PR_EL1 Exit=PR_MAC_NM_R
ITO LOP_LRO_A  Add     El1=PR_MAC_NM_R El2=C_1 Exit=RA_TMP2
ITO LOP_LRO  Write El1=RA_TMP2 El2=Move
ITO LOP_LRA_A  Add     El1=PR_MAC_NM_R El2=SLOT_E1 Exit=RA_TMP
ITO LOP_LRA    Write El1=RA_TMP El2=PR_MAC_ARG2
ITO LOP_LRT_A  Add     El1=PR_MAC_NM_R El2=SLOT_EXIT Exit=RA_TMP
ITO LOP_LRT    Write El1=RA_TMP El2=RA_LM_REL
/name_T: Move exit_lux → RA_LM_EXIT (stored in NM_K slot)
RVOCA LOP_MKT PS_ALLOC_SUBLUX
ITO LOP_SAVT Move El1=PR_EL1 Exit=PR_MAC_NM_K
ITO LOP_LTO_A  Add     El1=PR_MAC_NM_K El2=C_1 Exit=RA_TMP2
ITO LOP_LTO  Write El1=RA_TMP2 El2=Move
ITO LOP_LTA_A  Add     El1=PR_MAC_NM_K El2=SLOT_E1 Exit=RA_TMP
ITO LOP_LTA    Write El1=RA_TMP El2=PR_MAC_ARG3
ITO LOP_LTT_A  Add     El1=PR_MAC_NM_K El2=SLOT_EXIT Exit=RA_TMP
ITO LOP_LTT    Write El1=RA_TMP El2=RA_LM_EXIT
/name_J: Voca → PR_LOP_LUMENOP (ADD_LUMEN or REMOVE_LUMEN)
RVOCA LOP_MKJ PS_ALLOC_SUBLUX
ITO LOP_SAVJ Move El1=PR_EL1 Exit=PR_MAC_NM_J
ITO LOP_LJO_A  Add     El1=PR_MAC_NM_J El2=C_1 Exit=RA_TMP2
ITO LOP_LJO  Write El1=RA_TMP2 El2=Voca
ITO LOP_LJA_A  Add     El1=PR_MAC_NM_J El2=SLOT_E1 Exit=RA_TMP
ITO LOP_LJA    Write El1=RA_TMP El2=PR_LOP_LUMENOP
ITO LOP_LJX_A  Add     El1=PR_MAC_NM_J El2=SLOT_EXIT Exit=RA_TMP
ITO LOP_LJX    Write El1=RA_TMP El2=RA_LINK
/chain
ITO LOP_FIRST Move El1=PR_MAC_NAME Exit=PR_AN_FIRST
ITO LOP_LAST  Move El1=PR_MAC_NM_J Exit=PR_AN_LAST
RREDI LOP_RET_r

── LH / LR / LT / LX — unified via PS_MK_GL_PATTERN ────────
//All four: name(Move src→input_reg) → name_J(Voca SR_GL*) → name_W(Move RA_SR_OUT→exit_lux)
LH: input=RA_SR_LUX,   func=SR_GLL
LR: input=RA_SR_LUMEN, func=SR_GLR
LT: input=RA_SR_LUMEN, func=SR_GLE
LX: input=RA_SR_LUMEN, func=SR_GLX//
RVOCA PS_LINE_LH PS_READ_NAME_MOF
RVOCA CG_PL_LH_SRC  PS_NEXT_SYMBOL
ITO PS_LH_SAVS  Move El1=PR_SYM    Exit=PR_MAC_ARG1
RVOCA CG_PL_LH_TGT  PS_NEXT_SYMBOL
ITO PS_LH_SAVT  Move El1=PR_SYM    Exit=PR_MAC_ARG2
ITO PS_LH_SFUNC Move El1=SR_GLL    Exit=PR_GL_SRFUNC
ITO PS_LH_STGT  Move El1=RA_SR_LUX Exit=PR_GL_NAMETGT
RVOCA PS_LH_PAT   PS_MK_GL_PATTERN
ITO PS_LH_DONE3 Jump Exit=PS_INSTR_DONE

RVOCA PS_LINE_LR PS_READ_NAME_MOF
RVOCA CG_PL_LR_SRC  PS_NEXT_SYMBOL
ITO PS_LR_SAVS  Move El1=PR_SYM      Exit=PR_MAC_ARG1
RVOCA CG_PL_LR_TGT  PS_NEXT_SYMBOL
ITO PS_LR_SAVT  Move El1=PR_SYM      Exit=PR_MAC_ARG2
ITO PS_LR_SFUNC Move El1=SR_GLR      Exit=PR_GL_SRFUNC
ITO PS_LR_STGT  Move El1=RA_SR_LUMEN Exit=PR_GL_NAMETGT
RVOCA PS_LR_PAT   PS_MK_GL_PATTERN
ITO PS_LR_DONE3 Jump Exit=PS_INSTR_DONE

RVOCA PS_LINE_LT PS_READ_NAME_MOF
RVOCA CG_PL_LT_SRC  PS_NEXT_SYMBOL
ITO PS_LT_SAVS  Move El1=PR_SYM      Exit=PR_MAC_ARG1
RVOCA CG_PL_LT_TGT  PS_NEXT_SYMBOL
ITO PS_LT_SAVT  Move El1=PR_SYM      Exit=PR_MAC_ARG2
ITO PS_LT_SFUNC Move El1=SR_GLE      Exit=PR_GL_SRFUNC
ITO PS_LT_STGT  Move El1=RA_SR_LUMEN Exit=PR_GL_NAMETGT
RVOCA PS_LT_PAT   PS_MK_GL_PATTERN
ITO PS_LT_DONE3 Jump Exit=PS_INSTR_DONE

RVOCA PS_LINE_LX PS_READ_NAME_MOF
RVOCA CG_PL_LX_SRC  PS_NEXT_SYMBOL
ITO PS_LX_SAVS  Move El1=PR_SYM      Exit=PR_MAC_ARG1
RVOCA CG_PL_LX_TGT  PS_NEXT_SYMBOL
ITO PS_LX_SAVT  Move El1=PR_SYM      Exit=PR_MAC_ARG2
ITO PS_LX_SFUNC Move El1=SR_GLX      Exit=PR_GL_SRFUNC
ITO PS_LX_STGT  Move El1=RA_SR_LUMEN Exit=PR_GL_NAMETGT
RVOCA PS_LX_PAT   PS_MK_GL_PATTERN
ITO PS_LX_DONE3 Jump Exit=PS_INSTR_DONE

── PS_MK_GL_PATTERN — build 3-Lux lumen-walk sequence ────────
//IN:  PR_MAC_NAME, PR_MAC_ARG1(src), PR_MAC_ARG2(tgt)
PR_GL_SRFUNC = SR_GL* subroutine, PR_GL_NAMETGT = input register
Structure: name(Move src→input) → name_J(Voca SR_GL*) → name_W(Move SR_OUT→exit_lux)
Non-leaf — RA_LINK is saved/restored automatically by the call stack.//
NOLINK
/name_W: Move RA_SR_OUT → exit_lux
RVOCA PS_MK_GL_PATTERN PS_ALLOC_SUBLUX
ITO GL_SAVW Move El1=PR_EL1 Exit=PR_MAC_NM_R
ITO GL_LWO_A  Add     El1=PR_MAC_NM_R El2=C_1 Exit=RA_TMP2
ITO GL_LWO  Write El1=RA_TMP2 El2=Move
ITO GL_LWA_A  Add     El1=PR_MAC_NM_R El2=SLOT_E1 Exit=RA_TMP
ITO GL_LWA    Write El1=RA_TMP El2=RA_SR_OUT
ITO GL_LWT_A  Add     El1=PR_MAC_NM_R El2=SLOT_EXIT Exit=RA_TMP
ITO GL_LWT    Write El1=RA_TMP El2=PR_MAC_ARG2
/name_J: Voca El1=SR_GL* Exit=RA_LINK
RVOCA GL_MKJ PS_ALLOC_SUBLUX
ITO GL_SAVJ Move El1=PR_EL1 Exit=PR_MAC_NM_J
ITO GL_LJO_A  Add     El1=PR_MAC_NM_J El2=C_1 Exit=RA_TMP2
ITO GL_LJO  Write El1=RA_TMP2 El2=Voca
ITO GL_LJA_A  Add     El1=PR_MAC_NM_J El2=SLOT_E1 Exit=RA_TMP
ITO GL_LJA    Write El1=RA_TMP El2=PR_GL_SRFUNC
/Exit=RA_LINK
ITO GL_LJX_A  Add     El1=PR_MAC_NM_J El2=SLOT_EXIT Exit=RA_TMP
ITO GL_LJX    Write El1=RA_TMP El2=RA_LINK
/name: Move src → input_reg
ITO GL_LNO_A  Add     El1=PR_MAC_NAME El2=C_1 Exit=RA_TMP2
ITO GL_LNO  Write El1=RA_TMP2 El2=Move
ITO GL_LNA_A  Add     El1=PR_MAC_NAME El2=SLOT_E1 Exit=RA_TMP
ITO GL_LNA    Write El1=RA_TMP El2=PR_MAC_ARG1
ITO GL_LNT_A  Add     El1=PR_MAC_NAME El2=SLOT_EXIT Exit=RA_TMP
ITO GL_LNT    Write El1=RA_TMP El2=PR_GL_NAMETGT
ITO GL_FIRST Move El1=PR_MAC_NAME Exit=PR_AN_FIRST
ITO GL_LAST  Move El1=PR_MAC_NM_R Exit=PR_AN_LAST
RREDI GL_RET_r
── RCALL_AT macro: RCALL_AT name sub landing ────────────────
//name: Move landing → RA_LINK; name_J: Jump Exit=sub
Identical to CALL_AT structure → reuse PS_MK_RCALL_PATTERN.//
RVOCA PS_LINE_RCAT PS_READ_NAME_MOF
RVOCA CG_PL_RCAT_SB  PS_NEXT_SYMBOL
ITO PS_RCAT_SAVSB Move   El1=PR_SYM     Exit=PR_MAC_ARG1
RVOCA CG_PL_RCAT_LD  PS_NEXT_SYMBOL
ITO PS_RCAT_SAVLD  Move  El1=PR_SYM     Exit=PR_MAC_ARG2
ITO PS_RCAT_SLND   Move  El1=PR_MAC_ARG2 Exit=PR_CAL_LANDING
RVOCA PS_RCAT_PAT    PS_MK_RCALL_PATTERN
ITO PS_RCAT_DONE   Move  El1=PR_MAC_NAME Exit=PR_AN_FIRST
ITO PS_RCAT_DONE2  Move  El1=PR_MAC_NM_J Exit=PR_AN_LAST
ITO PS_RCAT_DONE3  Jump  Exit=PS_INSTR_DONE

── YAKU_NEXO / YAKU_NEXO_TERM / YAKU_NEXO_ALIAS ─────────────
//YAKU_NEXO aspect body    → NEW Yaku_aspect; ForType lumen; build RO graph from body (no quotes)
YAKU_NEXO_TERM aspect body  → YAKU_NEXO + Terminates lumen
YAKU_NEXO_ALIAS aspect alias body → YAKU_NEXO_TERM + second ForType lumen
Strategy: tbuf[9] distinguishes variants ('_'=has suffix, 0=plain YAKU_NEXO)
tbuf[9]=0 → YAKU_NEXO (9 chars: S-A-K-U-_-N-E-X-O)
tbuf[9]='_' + tbuf[10]='T' → YAKU_NEXO_TERM
tbuf[9]='_' + tbuf[10]='A' → YAKU_NEXO_ALIAS//
NEW PR_DEF_SUBTYPE   /0=YAKU_NEXO, 1=YAKU_NEXO_TERM, 2=YAKU_NEXO_ALIAS (YAKU_NEXO_CMP/ARITH not yet in parser.re)
NEW PR_YAKU_RULE_LUX  /the Yaku_aspect Lux ID
NEW PR_DEF_HASH_SAVE /saved hash for Yaku_aspect insertion

/PS_LINE_SAKU: entry from S-dispatch (tbuf[1]=='A' → YAKU)
NOLINK
── YAKU_NEXO commands (direct dispatch via hash) ────────────
NOLINK
CLEAR PS_LINE_SAKU PR_DEF_SUBTYPE
ITO PS_SAKU_JMP Jump Exit=PS_DEF_COMMON
NOLINK
ITO PS_LINE_SAKU_TERM  Move El1=C_1 Exit=PR_DEF_SUBTYPE
ITO PS_SAKU_TERM2      Jump Exit=PS_DEF_COMMON
NOLINK
ITO PS_LINE_SAKU_ALIAS Move El1=C_2 Exit=PR_DEF_SUBTYPE
ITO PS_SAKU_ALIAS2     Jump Exit=PS_DEF_COMMON

/PS_DEF_COMMON: read aspect name token
RVOCA PS_DEF_COMMON PS_NEXT_SYMBOL
/PR_SYM = aspect Lux ID; save it
ITO PS_DEF_SAVNM Move     El1=PR_SYM     Exit=PR_MAC_ARG1
/For YAKU_NEXO_ALIAS: read alias name
JEQ PS_DEF_ALIASCK PR_DEF_SUBTYPE C_2 PS_DEF_READ_ALIAS
ITO PS_DEF_NOALIAS Jump   Exit=PS_DEF_STR
/read alias
RVOCA PS_DEF_READ_ALIAS PS_NEXT_SYMBOL
ITO PS_DEF_SAVAL Move     El1=PR_SYM     Exit=PR_MAC_ARG2
/PS_DEF_STR: create Yaku_aspect Lux
RVOCA PS_DEF_STR ALLOC_LUX
ITO PS_DEF_SAVRULE Move   El1=RA_ALLOC_RESULT Exit=PR_YAKU_RULE_LUX
/link Yaku_aspect --ForType--> aspect
LINK_OP PS_DEF_LNKFT PR_YAKU_RULE_LUX ForType PR_MAC_ARG1
/if YAKU_NEXO_ALIAS: also LINK --ForType--> alias
JEQ PS_DEF_ALCK2 PR_DEF_SUBTYPE C_2 PS_DEF_LNKAL
ITO PS_DEF_NOAL2 Jump     Exit=PS_DEF_TERMCK
LINK_OP PS_DEF_LNKAL PR_YAKU_RULE_LUX ForType PR_MAC_ARG2
/if YAKU_NEXO_TERM or ALIAS: LINK --Terminates--> Yaku
JZ PS_DEF_TERMCK PR_DEF_SUBTYPE PS_DEF_READSTR
LINK_OP PS_DEF_LNKTERM PR_YAKU_RULE_LUX Terminates Yaku
── Graph-builder registers (used by PS_DEF_BUILD_GRAPH) ──────
NEW PR_GB_FIRST      /first RO_Lux of rule graph (= Rule.word via SETREF)
NEW PR_GB_PREV       /previously created RO_Lux (for Next-chaining)
NEW PR_GB_LIT_FIRST  /first Lux of current literal byte-chain
NEW PR_GB_LIT_PREV   /previous Lux of current literal byte-chain (for Next-chain)
NEW PR_GB_LITWORD    /accumulating packed word for literal
NEW PR_GB_LITSHIFT   /current bit-shift position in PR_GB_LITWORD (0,8,16,...56)
NEW PR_GB_PHBUF      /packed first-8-bytes of placeholder name (for dispatch)
NEW PR_GB_PHI        /byte counter while reading placeholder name
NEW PR_GB_NEWLUX    /saved new RO_Lux ID across LINK_OP calls (RA_ALLOC_RESULT is volatile)

//Build the RO_* graph instead of storing the string.
Replace PS_ST_SKIP call with PS_DEF_BUILD_GRAPH.
PS_DEF_BUILD_GRAPH reads the "..." string at lpos and builds RO_* Lux chain.
After building: Yaku_name.word = SETREF to first RO_Lux (via Write Yaku_lux first_ro).
YAKU_NEXO: body is rest of line (no quotes) — use PGB_BODY_START entry//
ITO PS_DEF_READSTR Move   El1=PR_YAKU_RULE_LUX Exit=PR_EL1
RCALL_AT PS_DEF_SETRET PS_DEF_BUILD_BODY PS_DEF_DONE
ITO PS_DEF_DONE Jump      Exit=PS_LINE_DONE

/PS_DEF_BUILD_BODY — entry for YAKU_NEXO: body without quotes (real definition below near PGB_LOOP)

============================================================
//PS_DEF_BUILD_GRAPH — build RO_* Lux graph from "template" string
Called after PS_DEF_RULE_LUX is set; PR_EL1 = Yaku_name Lux.
lpos currently points at (or before) the opening '"'.
After building: Write PR_EL1 ← ID of first RO_Lux (via PR_GB_FIRST).

Graph: RO_Lux luces linked by Next. Each lux has Op = RO_* type.
Literals → packed into byte-chain (8 bytes/lux, little-endian).
{v1}/{result}/etc → escape bytes 0x01-0x13.
~ → LF (0x0A). Closing " ends string.

Non-leaf — RA_LINK is saved/restored automatically by the call stack.//
============================================================
//============================================================
PS_DEF_BUILD_GRAPH / PS_DEF_BUILD_BODY
Replaces RO_* graph builder. Now builds packed string:
- Literal bytes packed 8 per lux, little-endian
- Placeholders {name} → escape byte (0x01..0x13)
- NUL terminator lux at end
- aether[PR_EL1] = first lux addr

Escape byte map:
  {v1}→1 {v2}→2 {result}→3 {xlen}→4 {fresh}→5 {fresh2}→6
  {ptr_tgt}→7 {ldest}→8 {lnext}→9 {lnext_id}→11 {totalsz}→12
  {flb}→14 {vx0}→15 {vx1}→16 {vx2}→17 {vx3}→18 {vx8}→19
  ~ → LF (0x0A)
  Closing " → end of string
//============================================================

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
ALLOC_TO PGB_ALLOC_LUX PGB_PS_LUX C_1
/If first lux: record as PGB_PS_FIRST
JZ PGB_AC_FCK PGB_PS_FIRST PGB_AC_DONE
ITO PGB_AC_SETF       Move  El1=RA_ALLOC_RESULT Exit=PGB_PS_FIRST
NOLINK
RREDI PGB_AC_DONE
/── PGB_FLUSH_WORD: write PGB_PS_WORD into PGB_PS_LUX, reset ─
NOLINK
RVOCA PGB_FLUSH_WORD    PGB_ALLOC_LUX
ITO PGB_FW_WRITE      Write El1=PGB_PS_LUX    El2=PGB_PS_WORD
CLEAR PGB_FW_RST_W PGB_PS_WORD
CLEAR PGB_FW_RST_S PGB_PS_SHIFT
RREDI PGB_FW_RET

/── PGB_EMIT_BYTE: pack one byte into current word ───────────
/Flushes word when shift==64. Does NOT alloc lux until flush.
/IN: RA_TMP = byte value
NOLINK
/Pack: word |= byte << shift
ITO PGB_EMIT_BYTE        Left  El1=RA_TMP         El2=PGB_PS_SHIFT  Exit=RA_TMP2
ITO PGB_EB_OR         Or    El1=PGB_PS_WORD    El2=RA_TMP2       Exit=PGB_PS_WORD
ITO PGB_EB_SINC       Add   El1=PGB_PS_SHIFT   El2=C_8           Exit=PGB_PS_SHIFT
/If shift==64: flush
JEQ PGB_EB_FCK PGB_PS_SHIFT C_64 PGB_EB_FLUSH
RREDI PGB_EB_RET
NOLINK
RVOCA PGB_EB_FW       PGB_FLUSH_WORD
RREDI PGB_EB_FRET

/── PGB_FINALIZE: flush partial word + write NUL terminator ──
NOLINK
/Flush partial word if shift > 0
JZ PGB_FINALIZE PGB_PS_SHIFT PGB_FN_NUL
RVOCA PGB_FN_FW       PGB_FLUSH_WORD
/NUL terminator lux (word=0)
NOLINK
CLEAR PGB_FN_NUL PGB_PS_WORD
RVOCA PGB_FN_NALLOC   PGB_ALLOC_LUX
ITO PGB_FN_NWRITE     Write El1=PGB_PS_LUX    El2=C_0
RREDI PGB_FN_RET

/── PGB_PLACEHOLDER: read {name}, emit escape byte ───────────
/Current byte after '{' is at PR_LBUF+PR_LPOS.
/Reads until '}', matches against known names, emits escape.
/Unknown → emits '?' (0x3F).
NEW PGB_PH_NAME0   /first 8 bytes of name packed
NEW PGB_PH_NLEN    /name length
NEW PGB_PH_SHIFT2  /packing shift
NOLINK
CLEAR PGB_PLACEHOLDER PGB_PH_NAME0
CLEAR PGB_PH_INIT1 PGB_PH_NLEN
CLEAR PGB_PH_INIT2 PGB_PH_SHIFT2
/Read bytes until '}'
JEQ PGB_PH_LOOP PR_LPOS PR_LLEN PGB_PH_DISPATCH
ITO PGB_PH_RADDR     Add   El1=PR_LBUF        El2=PR_LPOS       Exit=RA_TMP
ITO PGB_PH_RBYTE     Read  El1=RA_TMP         Exit=RA_TMP
ITO PGB_PH_ADV       Add   El1=PR_LPOS        El2=C_1           Exit=PR_LPOS
JEQ PGB_PH_RBJ RA_TMP RBRACE PGB_PH_DISPATCH
/Pack into name0 (first 8 bytes)
ITO PGB_PH_LENCK     ULess El1=PGB_PH_NLEN    El2=C_8           Exit=RA_TMP2
ITO PGB_PH_LENJ      JumpIf El1=RA_TMP2       Exit=PGB_PH_PACK
ITO PGB_PH_SKIP      Jump  Exit=PGB_PH_LOOP
NOLINK
ITO PGB_PH_PACK      Left  El1=RA_TMP         El2=PGB_PH_SHIFT2 Exit=RA_TMP2
ITO PGB_PH_OR        Or    El1=PGB_PH_NAME0   El2=RA_TMP2       Exit=PGB_PH_NAME0
ITO PGB_PH_SINC      Add   El1=PGB_PH_SHIFT2  El2=C_8           Exit=PGB_PH_SHIFT2
ITO PGB_PH_INC       Add   El1=PGB_PH_NLEN    El2=C_1           Exit=PGB_PH_NLEN
ITO PGB_PH_JMP       Jump  Exit=PGB_PH_LOOP
/Dispatch: compare PGB_PH_NAME0 against known names (packed little-endian)
/Names: "v1"=0x3176, "v2"=0x3276, "result"=..., etc.
NOLINK
RREDI PGB_PH_DISPATCH
/Compare name0 against packed strings
/v1 = 'v'=0x76, '1'=0x31 → packed LE = 0x3176
NEWSET PGB_N_V1      12662     /0x3176 = "v1" packed
NEWSET PGB_N_V2      12918     /0x3276 = "v2" packed
/result = 0x746C7573657266  (r=0x72,e=0x65,s=0x73,u=0x75,l=0x6C,t=0x74)
NEWSET PGB_N_RESULT  128009175786866 /0x746C75736572 = "result" packed
/fresh = 0x687365726600 (f=0x66,r=0x72,e=0x65,s=0x73,h=0x68)
NEWSET PGB_N_FRESH   448612627046 /0x6873657266 = "fresh" packed
/xlen = 0x6E656C78 (x=0x78,l=0x6C,e=0x65,n=0x6E)
NEWSET PGB_N_XLEN    1852140664 /0x6E656C78 = "xlen" packed
/ldest = 0x74736564_6C (l=0x6C,d=0x64,e=0x65,s=0x73,t=0x74)
NEWSET PGB_N_LDEST   500152231020 /0x747365646C = "ldest" packed
/lnext = 0x7478656E6C (l=0x6C,n=0x6E,e=0x65,x=0x78,t=0x74)
NEWSET PGB_N_LNEXT   500236119660 /0x7478656E6C = "lnext" packed
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
ITO PGB_PH_UNK   Move  El1=QMARK  Exit=RA_TMP
RVOCA PGB_PH_UEB  PGB_EMIT_BYTE
RREDI PGB_PH_URET
/── Escape byte emitters ──────────────────────────────────────────────────
FOR PGB_PH_E1 PGB_PH_E2 PGB_PH_E3 PGB_PH_E4 PGB_PH_E5 PGB_PH_E6 PGB_PH_E7
    NOLINK
    ITO {X}    Move  El1=C_{N}   Exit=RA_TMP
    RVOCA {X}B PGB_EMIT_BYTE
    RREDI {X}R
FOR PGB_PH_E8 PGB_PH_E9 PGB_PH_E10 PGB_PH_E11 PGB_PH_E12 PGB_PH_E13 PGB_PH_E14
    NOLINK
    ITO {X}    Move  El1=C_{N}   Exit=RA_TMP
    RVOCA {X}B PGB_EMIT_BYTE
    RREDI {X}R
FOR PGB_PH_E15 PGB_PH_E16 PGB_PH_E17 PGB_PH_E18 PGB_PH_E19
    NOLINK
    ITO {X}    Move  El1=C_{N}   Exit=RA_TMP
    RVOCA {X}B PGB_EMIT_BYTE
    RREDI {X}R

/── Main loop ─────────────────────────────────────────────────
/PS_DEF_BUILD_GRAPH entry: skip to opening '"', then build
NOLINK
NEWREF PS_DEF_BUILD_GRAPH PGB_SKIPWS  /alias/
/FOR loop currently emits nothing ({N} placeholder unresolved,
/pre-existing issue, same family as other {N}-templated macro bodies)./
FOR PGB_PS_FIRST PGB_PS_WORD PGB_PS_SHIFT
    CLEAR PGB_INIT_STATE{N} {X}
/Skip spaces to '"'
JEQ PGB_SKIPWS PR_LPOS PR_LLEN PGB_DONE
ITO PGB_PEEKR    Add    El1=PR_LBUF El2=PR_LPOS     Exit=RA_TMP
ITO PGB_PEEKV    Read   El1=RA_TMP                  Exit=RA_TMP
SWITCH RA_TMP
    SP     PGB_SKIPADV
    TAB    PGB_SKIPADV
    DQUOTE PGB_STR_START
ITO PGB_NOQUOTE  Jump   Exit=PGB_DONE
ITO PGB_SKIPADV  Add    El1=PR_LPOS El2=C_1         Exit=PR_LPOS
ITO PGB_SKIPLB   Jump   Exit=PGB_SKIPWS

/PS_DEF_BUILD_BODY: YAKU_NEXO body starts at lpos (no quotes)
NOLINK
/FOR loop currently emits nothing ({N} placeholder unresolved,
/pre-existing issue, same family as other {N}-templated macro bodies)./
FOR PGB_PS_FIRST PGB_PS_WORD PGB_PS_SHIFT
    CLEAR PGB_BB_INIT{N} {X}
ITO PS_DEF_BUILD_BODY  Jump  Exit=PGB_LOOP

ITO PGB_STR_START Add  El1=PR_LPOS El2=C_1 Exit=PR_LPOS /skip '"'
/Main byte loop
NOLINK
JEQ PGB_LOOP PR_LPOS PR_LLEN PGB_FLUSH_FINAL
ITO PGB_BREAD    Add    El1=PR_LBUF El2=PR_LPOS     Exit=RA_TMP
ITO PGB_BLOAD    Read   El1=RA_TMP                  Exit=PR_ST_BYTE
ITO PGB_BINCL    Add    El1=PR_LPOS El2=C_1         Exit=PR_LPOS
/Closing '"' → done
JEQ PGB_DQEND PR_ST_BYTE DQUOTE PGB_FLUSH_FINAL
/~ → LF escape (0x0A)
JEQ PGB_TILDECK PR_ST_BYTE TILDE PGB_EMIT_LF
/'{' → placeholder
JEQ PGB_BRACECK PR_ST_BYTE LBRACE PGB_EMIT_PH
/Ordinary byte → emit
ITO PGB_LITLIT   Move   El1=PR_ST_BYTE Exit=RA_TMP
RVOCA PGB_LITEB  PGB_EMIT_BYTE
ITO PGB_LITLB    Jump   Exit=PGB_LOOP
/LF placeholder
NOLINK
ITO PGB_EMIT_LF  Move   El1=C_10  Exit=RA_TMP
RVOCA PGB_LFEB   PGB_EMIT_BYTE
ITO PGB_LFLB     Jump   Exit=PGB_LOOP
/Placeholder {name}
NOLINK
RVOCA PGB_PHCALL PGB_PLACEHOLDER
ITO PGB_PHLB     Jump   Exit=PGB_LOOP
/Flush + store result
NOLINK
RVOCA PGB_FF_FIN  PGB_FINALIZE
/Store first lux addr into aether[PR_EL1]
ITO PGB_DONE     Write El1=PR_EL1 El2=PGB_PS_FIRST
RREDI PGB_DONE_r

── WALK_ONE macro: WALK_ONE name lux_val rel_val ─────────────
//name_LUX: Move lux_val → RA_SR_LUX
name_REL: Move rel_val → RA_SR_REL
RVOCA name SR_WALK_ONE
── WALK_ONE macro: WALK_ONE name lux_val rel_val ─────────────
name_LUX: Move lux_val → RA_SR_LUX
name_REL: Move rel_val → RA_SR_REL
name: Voca El1=SR_WALK_ONE
Unified via PS_MK_WO_PATTERN.//
RVOCA PS_LINE_WALK PS_READ_NAME_MOF
RVOCA CG_PL_WO_LX  PS_NEXT_SYMBOL
ITO PS_WO_SAVLX  Move El1=PR_SYM Exit=PR_MAC_ARG1
RVOCA CG_PL_WO_RL  PS_NEXT_SYMBOL
ITO PS_WO_SAVRL  Move El1=PR_SYM Exit=PR_MAC_ARG2
RVOCA PS_WO_PAT    PS_MK_WO_PATTERN
ITO PS_WO_DONE   Move El1=PR_MAC_NM_R  Exit=PR_AN_FIRST
ITO PS_WO_DONE2  Move El1=PR_MAC_NAME  Exit=PR_AN_LAST
ITO PS_WO_DONE3  Jump Exit=PS_INSTR_DONE

── PS_MK_WO_PATTERN — build WALK_ONE 3-Lux sequence ──────────
//IN:  PR_MAC_NAME, PR_MAC_ARG1(lux_val), PR_MAC_ARG2(rel_val)
Builds: name_LUX(Move arg1→SR_LUX) → name_REL(Move arg2→SR_REL) → name(Voca SR_WALK_ONE)
Non-leaf — RA_LINK is saved/restored automatically by the call stack.//
NOLINK
/name_LUX (stored in NM_R): Move arg1 → RA_SR_LUX
RVOCA PS_MK_WO_PATTERN PS_ALLOC_SUBLUX
ITO WOP_SAVL Move El1=PR_EL1 Exit=PR_MAC_NM_R
ITO WOP_LLO_A  Add     El1=PR_MAC_NM_R El2=C_1 Exit=RA_TMP2
ITO WOP_LLO  Write El1=RA_TMP2 El2=Move
ITO WOP_LLA_A  Add     El1=PR_MAC_NM_R El2=SLOT_E1 Exit=RA_TMP
ITO WOP_LLA    Write El1=RA_TMP El2=PR_MAC_ARG1
ITO WOP_LLT_A  Add     El1=PR_MAC_NM_R El2=SLOT_EXIT Exit=RA_TMP
ITO WOP_LLT    Write El1=RA_TMP El2=RA_SR_LUX
/name_REL (stored in NM_K): Move arg2 → RA_SR_REL
RVOCA WOP_MKR PS_ALLOC_SUBLUX
ITO WOP_SAVR Move El1=PR_EL1 Exit=PR_MAC_NM_K
ITO WOP_LRO_A  Add     El1=PR_MAC_NM_K El2=C_1 Exit=RA_TMP2
ITO WOP_LRO  Write El1=RA_TMP2 El2=Move
ITO WOP_LRA_A  Add     El1=PR_MAC_NM_K El2=SLOT_E1 Exit=RA_TMP
ITO WOP_LRA    Write El1=RA_TMP El2=PR_MAC_ARG2
ITO WOP_LRT_A  Add     El1=PR_MAC_NM_K El2=SLOT_EXIT Exit=RA_TMP
ITO WOP_LRT    Write El1=RA_TMP El2=RA_SR_REL
/name: Voca El1=SR_WALK_ONE Exit=RA_LINK
ITO WOP_LNO_A  Add     El1=PR_MAC_NAME El2=C_1 Exit=RA_TMP2
ITO WOP_LNO  Write El1=RA_TMP2 El2=Voca
ITO WOP_LNA_A  Add     El1=PR_MAC_NAME El2=SLOT_E1 Exit=RA_TMP
ITO WOP_LNA    Write El1=RA_TMP El2=SR_WALK_ONE
ITO WOP_LNX_A  Add     El1=PR_MAC_NAME El2=SLOT_EXIT Exit=RA_TMP
ITO WOP_LNX    Write El1=RA_TMP El2=RA_LINK
/chain: name_LUX → name_REL → name
RREDI WOP_RET_r

/── Command handler registrations — lux.word = handler entrypoint
/Required for BS_LOOKUP dispatch: hash(name) → lux → Read lux.word → JumpReg
NEWREF NEW           PS_LINE_NEW
NEWREF SET           PS_LINE_SET
NEWREF LINK          PS_LINE_LNK
NEWREF LOAD          PS_LINE_LOAD
NEWREF NOLINK        PS_LINE_NOLINK
NEWREF YAKU_NEXO     PS_LINE_SAKU
NEWREF YAKU_NEXO_TERM  PS_LINE_SAKU_TERM
NEWREF YAKU_NEXO_ALIAS PS_LINE_SAKU_ALIAS

LINK PS_MAIN Entry Yaku
