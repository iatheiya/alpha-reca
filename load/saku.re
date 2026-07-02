//saku.re — Reca single-pass loader

PUBLIC ENTRY POINT: LOAD_MAIN (unchanged — external callers not affected)

Architecture: single-pass with backfill + lcount prepass
  Wave 0 (prepass): scan all files, count LINK lumina per src name
  Wave 1 (load):    single pass per file, build full graph
  Backfill:         forward references resolved after all files loaded

Naming conventions:
  LOAD_*     — actions that perform loading (public or protocol)
  SK_*       — Saku internal state (registers, buffers, scratch)
  BS_*       — shared primitives from lexer.re/intern.re (not redeclared here)

Depends on: lexer.re  (BS_READ_BYTE, BS_READ_TOKEN, BS_SKIP_TO_EOL,
             BS_TOKEN_VALUE, BS_SCAN_EQ, BS_PACK_TOKBUF, BS_PARSE_INT,
             BS_BLOCK_SKIP, BS_TOKBUF_BASE, BS_READBUF_BASE, RA_BS_TMP*)
            intern.re (BS_LOOKUP, BS_INTERN, BS_INTERN_NAMED,
             BS_HT_BASE/MASK/SIZE, BS_LAST_ITO, RA_BS_RESULT, RA_BS_EL*)
            alloc.re (ALLOC_LUCES, ADD_LUMEN, ALLOC_TO)
            registers.re (RA_MA0..RA_MA7, RA_MC_PREV, RA_LINK, RA_LINK_REF etc.)
            constants.re (C_0..C_10, C_33, C_64 etc.)

── LOAD_MAIN interface — input parameters ────────────────────
Written by caller (loader.py or future Reca loader) before calling LOAD_MAIN.
Convention matches SC_A0/SC_A1 style: caller places args, callee reads them.//
NEW BS_FILE_LIST   /addr of file path array (packed strings, stride=1)
NEW BS_FILE_COUNT  /number of files to load

── Saku internal state ───────────────────────────────────────
/File iteration
NEW SK_FIDX           /current file index
NEW SK_FPATH          /current file path addr (packed string)
/Block-comment state
NEW SK_BLOCK_CMT      /0=normal, 1=inside multi-line block comment
/Body buffer (FOR/SAVE template expansion)
NEWSET SK_BODY_BUF_SIZE 4096
BLOCK  SK_BODY_BUF    4096
SETREF SK_BODY_BUF SK_BODY_BUF
NEW    SK_BODY_BASE   /base addr of SK_BODY_BUF (set at init)
NEWREF LD_BODY_BUF_BASE_VAL SK_BODY_BASE  /alias: macros.re uses LD_BODY_BUF_BASE_VAL
NEW    SK_BODY_PTR    /write position
NEWREF LD_BODY_PTR SK_BODY_PTR  /alias: macros.re uses LD_BODY_PTR
NEW    SK_BODY_LIMIT  /base + size: overflow guard
NEW    SK_BODY_LINES  /number of lines in body buf
/Backfill buffer: (slot_addr, name_hash) pairs, 0-terminated
BLOCK  SK_BACKFILL_BUF  2048
SETREF SK_BACKFILL_BUF SK_BACKFILL_BUF
NEW    SK_BACKFILL_BASE
NEW    SK_BACKFILL_PTR
//Lcount table: [addr, count, prefix, addr, count, prefix, ..., 0] — max ~682 distinct src names
prefix: 0=unset, 1=Data lux, ITO_SIZE=ITO lux. Set by LOAD_CMD_NEW/LOAD_CMD_ITO
during Wave 1; read by LOAD_APPLY_LINKS to size the offset for ADD_LUMEN.//
BLOCK  SK_LCOUNT_BUF  2048
SETREF SK_LCOUNT_BUF SK_LCOUNT_BUF
NEW    SK_LCOUNT_BASE
NEW    SK_LCOUNT_PTR
NEW    SK_LCG_PFADDR  /LOAD_LCOUNT_GET: addr of found entry's prefix slot (0 if miss)/
//Pending-LINK buffer: (src_symtab_addr, rel_symtab_addr, tgt_symtab_addr) triples,
0-terminated. Recorded by LOAD_CMD_LINK_IMPL during Wave 1 (symtab addrs may not
yet be resolved — forward refs). Applied by LOAD_APPLY_LINKS after Wave 1, once
every name's address and lux-kind prefix are known.//
BLOCK  SK_PLINK_BUF  2048
SETREF SK_PLINK_BUF SK_PLINK_BUF
NEW    SK_PLINK_BASE
NEW    SK_PLINK_PTR   /write cursor — used while recording in LOAD_CMD_LINK_IMPL/
NEW    SK_PLINK_ITER  /read cursor — used while applying in LOAD_APPLY_LINKS/
/Pack-string state (SET name "...")
NEW SK_PS_WORD        /accumulator u64 word
NEW SK_PS_SHIFT       /multiplier (1, 256, 65536 ... 256^7); wraps to 0 at 256^8
NEW SK_PS_FIRST       /addr of first allocated lux
NEW SK_PS_NAMEDST     /name lux to write result into
NEW SK_PS_ESCAPED     /flag: previous byte was backslash
/Scratch
NEW SK_TMP
NEWREF LD_TMP SK_TMP  /alias: macros.re uses LD_TMP as shared loader scratch
NEW SK_TMP2
NEW SK_TMP3
NEW SK_FLAG
SETREF SK_FLAG SK_FLAG  /self-ref: Equal writes here, JumpIf reads here
NEWREF LD_FLAG SK_FLAG  /alias: macros.re uses LD_FLAG
/Macro arg reading
NEW SK_MA_RESULT      /resolved addr from LOAD_MA_READARG (0=done)
NEW SK_MA_HASH        /hash of most recently read arg token
NEW SK_MA_HASH0       /hash of MA0 token (for _J/_K name extension)
NEW SK_ITO_ADDR       /current ITO lux addr
NEW SK_ITO_NEW        /1=new alloc, 0=existing
NEW SK_ITO_E1         /parsed El1 addr
NEW SK_ITO_E2         /parsed El2 addr
NEW SK_ITO_EXIT       /parsed Exit addr
NEW SK_ITO_LCNT       /lumen count from lcount prepass
/Backfill intern
NEW SK_INTERN_SLOT    /slot addr to fill (set before LOAD_INTERN call)
/Cmd dispatch
NEW SK_CMD_ADDR       /current command lux addr (set by LOAD_DISPATCH_LINE)
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
NEW SK_SES_SREG_1
NEW SK_SES_SREG_2
NEW SK_SES_SREG_3
NEW SK_SES_SREG_4
NEW SK_SES_SREG_5
NEW SK_SES_SREG_6
NEW SK_SES_SREG_7
/LOAD_INTERN_SETUP scratch
NEWREF SK_ALLOC_ITO_OP __LT_ALLOC_ITO  /word = addr(__LT_ALLOC_ITO); sentinel for detection
/LOAD_MA_READARG: integer → C_N address mapping
NEWREF SK_MA0_ADDR RA_MA0  /addr of RA_MA0 for builder detection
/Emit-int scratch
NEW SK_EIB_VAL
NEW SK_EIB_DIV
NEW SK_ABF_PTR
NEW SK_ABF_SLOT
NEW SK_ABF_HASH
NEW SK_PB_PTR
NEW SK_IRA_KEY1
NEW SK_IRA_KEY2
NEW SK_POS_SLOT       /positional slot index (1..7) for LOAD_READARG_KV

── LOAD_MAIN: public entry point ────────────────────────────
NOLINK
/Init body buffer
ITO LOAD_MAIN        Move  El1=SK_BODY_BUF      Exit=SK_BODY_BASE
ITO LOAD_MAIN_BL     Add   El1=SK_BODY_BASE     El2=SK_BODY_BUF_SIZE  Exit=SK_BODY_LIMIT
/Init backfill
CHAIN LOAD_MAIN_BF
    Move  El1=SK_BACKFILL_BUF  Exit=SK_BACKFILL_BASE
    Move  El1=SK_BACKFILL_BASE Exit=SK_BACKFILL_PTR
/Init lcount table
CHAIN LOAD_MAIN_LC
    Move  El1=SK_LCOUNT_BUF    Exit=SK_LCOUNT_BASE
    Move  El1=SK_LCOUNT_BASE   Exit=SK_LCOUNT_PTR
/Init pending-LINK buffer
CHAIN LOAD_MAIN_PLB
    Move  El1=SK_PLINK_BUF     Exit=SK_PLINK_BASE
    Move  El1=SK_PLINK_BASE    Exit=SK_PLINK_PTR
/Reset autolink
CLEAR LOAD_MAIN_MC RA_MC_PREV
── Wave 0: prepass — count LINK lumina per src name ─────────
CLEAR LOAD_MAIN_P0 SK_FIDX
JEQ LOAD_MAIN_PLOOP SK_FIDX BS_FILE_COUNT LOAD_MAIN_W1
ITO LOAD_MAIN_PFADDR Add   El1=BS_FILE_LIST     El2=SK_FIDX        Exit=SK_TMP
ITO LOAD_MAIN_PFLOAD Read  El1=SK_TMP           Exit=SK_FPATH
RVOCA LOAD_MAIN_PFF  LOAD_PREPASS_FILE
ITO LOAD_MAIN_PINC   Add   El1=SK_FIDX          El2=C_1            Exit=SK_FIDX
ITO LOAD_MAIN_PJMP   Jump  Exit=LOAD_MAIN_PLOOP
── Wave 1: load — build full graph ──────────────────────────
NOLINK
CLEAR LOAD_MAIN_W1 SK_FIDX
JEQ LOAD_MAIN_LOOP SK_FIDX BS_FILE_COUNT LOAD_MAIN_BF2
ITO LOAD_MAIN_FADDR  Add   El1=BS_FILE_LIST     El2=SK_FIDX        Exit=SK_TMP
ITO LOAD_MAIN_FLOAD  Read  El1=SK_TMP           Exit=SK_FPATH
RVOCA LOAD_MAIN_FILE LOAD_FILE
ITO LOAD_MAIN_INC    Add   El1=SK_FIDX          El2=C_1            Exit=SK_FIDX
ITO LOAD_MAIN_JMP    Jump  Exit=LOAD_MAIN_LOOP
/Apply backfill: resolve all forward references
NOLINK
RVOCA LOAD_MAIN_BF2  LOAD_APPLY_BACKFILL
/Apply deferred LINK lumina: now every name's address and prefix are known
RVOCA LOAD_MAIN_PL2  LOAD_APPLY_LINKS
RREDI LOAD_MAIN_RRET

── LOAD_OPEN_FILE: open SK_FPATH O_RDONLY, init read buffer ─
/OUT: RA_LOAD_FD = fd. Leaf.
CHAIN LOAD_OPEN_FILE
    Move  El1=SYS_OPENAT  Exit=SC_NR
    Move  El1=AT_FDCWD    Exit=SC_A0
    Move  El1=SK_FPATH    Exit=SC_A1
    Move  El1=O_RDONLY    Exit=SC_A2
ITO LOF_XR          Exire El1=C_0          El2=C_0   Exit=C_0
ITO LOF_FD          Move  El1=SC_A0        Exit=RA_LOAD_FD
CLEAR LOF_RPOS      RA_LOAD_RPOS
CLEAR LOF_RLEN      RA_LOAD_RLEN
RREDI LOF_RET

── LOAD_FILE: open, process, close one .re file ─────────────
NOLINK
RVOCA LOAD_FILE      LOAD_OPEN_FILE
CLEAR LOAD_FO_BCMT   SK_BLOCK_CMT
CLEAR LOAD_FO_IND    SK_IND_DEPTH
RVOCA LOAD_FL_LOOP   LOAD_DISPATCH_LINE
JZ LOAD_FL_EOFCK RA_LOAD_BYTE LOAD_FL_DONE
ITO LOAD_FL_JMP      Jump  Exit=LOAD_FL_LOOP
NOLINK
ITO LOAD_FL_DONE     Move  El1=SYS_CLOSE    Exit=SC_NR
ITO LOAD_FL_CLFD     Move  El1=RA_LOAD_FD   Exit=SC_A0
ITO LOAD_FL_CLXR     Exire El1=C_0          El2=C_0   Exit=C_0
RREDI LOAD_FL_RRET

── LOAD_DISPATCH_LINE: read first token and dispatch ─────────
/BS_READ_TOKEN handles whitespace, "//" inline blocks, "/" line comments.
/tlen==0 → EOF or comment → return.
NOLINK
RVOCA LOAD_DISPATCH_LINE  BS_READ_TOKEN
RVOCA LOAD_DL_CORE        LOAD_DISPATCH_CORE
RREDI LOAD_DL_RET

── LOAD_DISPATCH_CORE: dispatch whatever is already in BS_TOKBUF_BASE ─
//Shared by LOAD_DISPATCH_LINE (fresh-read-then-dispatch, the common
case) and LOAD_DISPATCH_BUILT_LINE (dispatch already-constructed
content, used by CHAIN body processing -- see BUGS.md: these
previously called LOAD_DISPATCH_LINE directly, which silently
discarded their carefully-built "ITO name op args" text by
immediately overwriting BS_TOKBUF_BASE with a fresh file-stream read).
IN: BS_TOKBUF_BASE/RA_LOAD_TLEN already set by the caller.//
NEWREF LOAD_DISPATCH_CORE LOAD_DC_TLENCK
NOLINK
JZ LOAD_DC_TLENCK RA_LOAD_TLEN LOAD_DC_RET
ITO LOAD_DC_FBTOK    Read  El1=BS_TOKBUF_BASE Exit=SK_TMP
/Lookup full token → SK_CMD_ADDR (for LOAD_CMD_UNKNOWN)
RVOCA LOAD_DC_LK     BS_LOOKUP
ITO LOAD_DC_SAVE     Move  El1=RA_BS_RESULT Exit=SK_CMD_ADDR
/Dispatch by first byte — SWITCH is O(1) via jump table
SWITCH SK_TMP
    ASCII_N  LOAD_CMD_N_GROUP
    ASCII_S  LOAD_CMD_SET_OR_SRF
    ASCII_L  LOAD_CMD_LINK_OR_LR
    ASCII_I  LOAD_CMD_ITO
    ASCII_B  LOAD_CMD_BLOCK
RVOCA LOAD_DC_UCMD   LOAD_CMD_UNKNOWN
RREDI LOAD_DC_RET

── LOAD_DISPATCH_BUILT_LINE: dispatch already-constructed content ────
//For CHAIN body processing: the build-name helpers write
"ITO name op args" into BS_TOKBUF_BASE and set RA_LOAD_TLEN to its
length; this dispatches THAT content directly. Activates redirect mode
(see registers.re/BS_READ_BYTE) so that nested BS_READ_TOKEN calls made
BY THE COMMAND HANDLER ITSELF (e.g. LOAD_CMD_ITO reading the name, then
the op, then El1=/El2=/Exit= via LOAD_ITO_READ_ARGS) also read from the
constructed text -- not just the first token. Without this, only the
outermost dispatch avoided the live-stream-reread bug; every handler's
OWN internal token reads still hit the live file stream, picking up
unrelated text from wherever the file position happened to be (since
the whole CHAIN body was already consumed into a separate buffer
by LOAD_READ_BODY before body-walking even began) -- see BUGS.md.//
NOLINK
ITO LOAD_DBL_RBASE   Move  El1=BS_TOKBUF_BASE Exit=RA_REDIRECT_BASE
ITO LOAD_DBL_RLEN    Move  El1=RA_LOAD_TLEN    Exit=RA_REDIRECT_LEN
CLEAR LOAD_DBL_RPOS  RA_REDIRECT_POS
RVOCA LOAD_DISPATCH_BUILT_LINE  LOAD_DISPATCH_CORE
NOLINK
CLEAR LOAD_DBL_ROFF  RA_REDIRECT_BASE
RREDI LOAD_DBL_RET

── LOAD_CMD_N_GROUP: dispatch N* commands ────────────────────
/Second byte: O→NOLINK, E+len3→NEW, E+len>3→macro (NEWREF/NEWSET/NEXO)
CHAIN
    LOAD_CMD_N_GROUP   Add   El1=BS_TOKBUF_BASE El2=C_1   Exit=SK_TMP
    LOAD_CNG_B1        Read  El1=SK_TMP         Exit=SK_TMP
SWITCH SK_TMP
    ASCII_O  LOAD_CMD_NOLINK
    ASCII_E  LOAD_CNG_NE
RVOCA LOAD_CNG_UCMD  LOAD_CMD_UNKNOWN
RREDI LOAD_CNG_URRET
NOLINK
JEQ LOAD_CNG_NE RA_LOAD_TLEN C_3 LOAD_CMD_NEW
RVOCA LOAD_CNG_LU    LOAD_CMD_UNKNOWN
RREDI LOAD_CNG_LRRET

── LOAD_CMD_NEW: NEW name ────────────────────────────────────
//Alloc 1 + 2*lcount + (lcount>0?1:0) luces.
If already in htable: no-op.//
NOLINK
RVOCA LOAD_CMD_NEW   BS_READ_TOKEN
JZ LOAD_CN_CKL RA_LOAD_TLEN LOAD_CN_DONE
RVOCA LOAD_CN_LCGET  LOAD_LCOUNT_GET
//Record this name's lux-kind prefix (C_1, Data lux) for later LINK application —
only if it has an lcount entry (lcount>0); a name never LINK'd needs none.//
JZ LOAD_CN_PFCK SK_LCG_PFADDR LOAD_CN_SZ1
ITO LOAD_CN_PFW      Write El1=SK_LCG_PFADDR  El2=C_1
/size = 1 + 2*lcount + (lcount>0?1:0)
ITO LOAD_CN_SZ1      Mul   El1=SK_TMP         El2=C_2             Exit=SK_FLAG
ITO LOAD_CN_SZ2      Add   El1=C_1            El2=SK_FLAG          Exit=RA_ALLOC_COUNT
JZ LOAD_CN_TCKZ SK_TMP LOAD_CN_INTERN
ITO LOAD_CN_TADD     Add   El1=RA_ALLOC_COUNT El2=C_1             Exit=RA_ALLOC_COUNT
RVOCA LOAD_CN_INTERN BS_LOOKUP
JZ LOAD_CN_LKCK RA_BS_RESULT LOAD_CN_ALLOC
RREDI LOAD_CN_DONE
NOLINK
RVOCA LOAD_CN_ALLOC  ALLOC_LUCES
/Save new lux addr before BS_PACK_TOKBUF clobbers RA_ALLOC_RESULT
ITO LOAD_CN_SAV      Move  El1=RA_ALLOC_RESULT Exit=RA_BS_EL0
/Pack token → packed string addr in RA_BS_TMP2 (for symbol table rebuild)
RVOCA LOAD_CN_PACK   BS_PACK_TOKBUF
/Restore lux addr, store packed name at lux[1]
ITO LOAD_CN_RST      Move  El1=RA_BS_EL0      Exit=RA_BS_RESULT
ITO LOAD_CN_N1A      Add   El1=RA_BS_RESULT   El2=C_1             Exit=SK_TMP
ITO LOAD_CN_N1W      Write El1=SK_TMP          El2=RA_BS_TMP2
/Insert into htable
CHAIN LOAD_CN_HH
    Move  El1=RA_LOAD_HASH   Exit=RA_HT_HASH
    Move  El1=RA_BS_RESULT   Exit=RA_HT_LID
    Move  El1=BS_HT_BASE     Exit=RA_HT_BASE
    Move  El1=BS_HT_MASK     Exit=RA_HT_MASK
    Move  El1=BS_HT_SIZE     Exit=RA_HT_SIZE
        RVOCA LOAD_CN_HT     HT_INSERT
RVOCA LOAD_CN_ASKP   BS_SKIP_TO_EOL
RREDI LOAD_CN_ARRET

── LOAD_CMD_SET_OR_SRF: SET / SETREF / SAVE ─────────────────
NOLINK
ITO LOAD_CMD_SET_OR_SRF  Add   El1=BS_TOKBUF_BASE El2=C_1  Exit=SK_TMP
ITO LOAD_CSS_B1           Read  El1=SK_TMP         Exit=SK_TMP
JEQ LOAD_CSS_CKE SK_TMP ASCII_E LOAD_CSS_SETCK
JEQ LOAD_CSS_CKA SK_TMP ASCII_A LOAD_CSS_SAVE
RVOCA LOAD_CSS_UCMD       LOAD_CMD_UNKNOWN
RREDI LOAD_CSS_RRET
NOLINK
JEQ LOAD_CSS_SETCK RA_LOAD_TLEN C_3 LOAD_CMD_SET_IMPL
RVOCA LOAD_CSS_SEUCMD     LOAD_CMD_UNKNOWN
RREDI LOAD_CSS_SERRET
NOLINK
RVOCA LOAD_CSS_SAVE       SAVE
RREDI LOAD_SAVE_RRET

── LOAD_CMD_SET_IMPL: SET name value ─────────────────────────
NOLINK
RVOCA LOAD_CMD_SET_IMPL  BS_READ_TOKEN
RVOCA LOAD_CS_NINT        BS_INTERN
ITO LOAD_CS_NADDR         Move  El1=RA_BS_RESULT  Exit=SK_TMP
RVOCA LOAD_CS_VTOK        BS_READ_TOKEN
/Detect quoted string ('"')
ITO LOAD_CS_VB0           Read  El1=BS_TOKBUF_BASE  Exit=SK_TMP2
JEQ LOAD_CS_QCK SK_TMP2 DQUOTE LOAD_CS_STR
/Detect integer: '-' or '0'..'9'
JEQ LOAD_CS_DASH SK_TMP2 MINUS LOAD_CS_INT
ITO LOAD_CS_D0            ULess El1=SK_TMP2          El2=ASCII_0  Exit=SK_FLAG
ITO LOAD_CS_D0J           JumpIf El1=SK_FLAG         Exit=LOAD_CS_SYM
ITO LOAD_CS_D9            ULess El1=ASCII_9          El2=SK_TMP2  Exit=SK_FLAG
ITO LOAD_CS_D9J           JumpIf El1=SK_FLAG         Exit=LOAD_CS_SYM
NOLINK
RVOCA LOAD_CS_INT         BS_PARSE_INT
ITO LOAD_CS_WINT          Write El1=SK_TMP           El2=RA_BS_PIVAL
RVOCA LOAD_CS_ISKLS       BS_SKIP_TO_EOL
RREDI LOAD_CS_IRRET
NOLINK
RVOCA LOAD_CS_SYM         BS_INTERN
ITO LOAD_CS_WSYM          Write El1=SK_TMP           El2=RA_BS_RESULT
RVOCA LOAD_CS_SSKLS       BS_SKIP_TO_EOL
RREDI LOAD_CS_SRRET
NOLINK
ITO LOAD_CS_STR           Move  El1=SK_TMP           Exit=SK_PS_NAMEDST
RVOCA LOAD_CS_STRP        LOAD_PACK_STRING
RVOCA LOAD_CS_SSKLS2      BS_SKIP_TO_EOL
RREDI LOAD_CS_SRRET2

── LOAD_PACK_STRING: read quoted string, pack 8 bytes/lux ────
//Called after BS_READ_TOKEN read opening '"'. Reads raw bytes until
unescaped '"'. Handles \n \t \\. Writes first lux addr into SK_PS_NAMEDST.//
NOLINK
CLEAR LOAD_PACK_STRING SK_PS_WORD
ITO LOAD_LPS_SINIT    Move  El1=C_1            Exit=SK_PS_SHIFT
CLEAR LOAD_LPS_CLRF   SK_PS_FIRST
CLEAR LOAD_LPS_ESC    SK_PS_ESCAPED
ITO LOAD_LPS_LOOP     Jump  Exit=LOAD_LPS_RB
NOLINK
RVOCA LOAD_LPS_RB     BS_READ_BYTE
JZ LOAD_LPS_EOFCK RA_LOAD_BYTE LOAD_LPS_DONE
/Handle escape
JZ LOAD_LPS_ESCCK SK_PS_ESCAPED LOAD_LPS_NOESC
/Previous was backslash: emit escape sequence
JEQ LOAD_LPS_ESCC RA_LOAD_BYTE ASCII_nl LOAD_LPS_ESC_LF
JEQ LOAD_LPS_ESCCT RA_LOAD_BYTE ASCII_tl LOAD_LPS_ESC_TAB
/Other: emit literally
CLEAR LOAD_LPS_ESCCLR SK_PS_ESCAPED
ITO LOAD_LPS_ESCLIT   Jump  Exit=LOAD_LPS_EMIT
NOLINK
ITO LOAD_LPS_ESC_LF   Move  El1=LF            Exit=RA_LOAD_BYTE
CLEAR LOAD_LPS_ESCLFX SK_PS_ESCAPED
ITO LOAD_LPS_ESCLJMP  Jump  Exit=LOAD_LPS_EMIT
NOLINK
ITO LOAD_LPS_ESC_TAB  Move  El1=TAB           Exit=RA_LOAD_BYTE
CLEAR LOAD_LPS_ESCTX  SK_PS_ESCAPED
ITO LOAD_LPS_ESCTJMP  Jump  Exit=LOAD_LPS_EMIT
NOLINK
JEQ LOAD_LPS_NOESC RA_LOAD_BYTE DQUOTE LOAD_LPS_DONE
JEQ LOAD_LPS_BSCK RA_LOAD_BYTE BACKSLASH LOAD_LPS_SETESC
ITO LOAD_LPS_EMIT     Jump  Exit=LOAD_LPS_EMITB
NOLINK
ITO LOAD_LPS_SETESC   Move  El1=C_1           Exit=SK_PS_ESCAPED
ITO LOAD_LPS_EJMP     Jump  Exit=LOAD_LPS_LOOP
NOLINK
/Emit byte: pack into current word (multiplier method)
ITO LOAD_LPS_EMITB    Mul   El1=RA_LOAD_BYTE  El2=SK_PS_SHIFT   Exit=SK_TMP
ITO LOAD_LPS_ADD      Add   El1=SK_PS_WORD    El2=SK_TMP         Exit=SK_PS_WORD
ITO LOAD_LPS_SMUL     Mul   El1=SK_PS_SHIFT   El2=C_256          Exit=SK_PS_SHIFT
JZ LOAD_LPS_SZCK SK_PS_SHIFT LOAD_LPS_FLUSH
ITO LOAD_LPS_JMP      Jump  Exit=LOAD_LPS_LOOP
/Flush: alloc 1 lux, write word, reset state
NOLINK
ITO LOAD_LPS_FLUSH    Move  El1=C_1            Exit=RA_ALLOC_COUNT
RVOCA LOAD_LPS_AC     ALLOC_LUCES
ITO LOAD_LPS_WW       Write El1=RA_ALLOC_RESULT El2=SK_PS_WORD
JZ LOAD_LPS_FCHK SK_PS_FIRST LOAD_LPS_SETF
ITO LOAD_LPS_FSKIP    Jump  Exit=LOAD_LPS_RESET
NOLINK
ITO LOAD_LPS_SETF     Move  El1=RA_ALLOC_RESULT Exit=SK_PS_FIRST
CLEAR LOAD_LPS_RESET  SK_PS_WORD
ITO LOAD_LPS_SRST     Move  El1=C_1            Exit=SK_PS_SHIFT
ITO LOAD_LPS_FJP      Jump  Exit=LOAD_LPS_LOOP
NOLINK
/Done: flush partial word (even if zero — null terminator lux)
ITO LOAD_LPS_DONE     Move  El1=C_1            Exit=RA_ALLOC_COUNT
RVOCA LOAD_LPS_DAC    ALLOC_LUCES
ITO LOAD_LPS_DWW      Write El1=RA_ALLOC_RESULT El2=SK_PS_WORD
JZ LOAD_LPS_DFCHK SK_PS_FIRST LOAD_LPS_DSETF
ITO LOAD_LPS_DSKIP    Jump  Exit=LOAD_LPS_WRITE
NOLINK
ITO LOAD_LPS_DSETF    Move  El1=RA_ALLOC_RESULT Exit=SK_PS_FIRST
/Write first lux addr into name lux
NOLINK
ITO LOAD_LPS_WRITE    Write El1=SK_PS_NAMEDST  El2=SK_PS_FIRST
RREDI LOAD_LPS_RRET

── LOAD_CMD_LINK_OR_LR: LINK / LR / LT / LX / LH ───────────
CHAIN
    LOAD_CMD_LINK_OR_LR   Add  El1=BS_TOKBUF_BASE El2=C_1  Exit=SK_TMP
    LOAD_CLR_B1           Read El1=SK_TMP          Exit=SK_TMP
        JEQ LOAD_CLR_CKI SK_TMP ASCII_I LOAD_CMD_LINK_IMPL
        RVOCA LOAD_CLR_UCMD  LOAD_CMD_UNKNOWN
        RREDI LOAD_CLR_RRET

── LOAD_CMD_LINK_IMPL: LINK src rel tgt ─────────────────────
//Deferred: records (src_symtab_addr, rel_symtab_addr, tgt_symtab_addr) into
SK_PLINK_BUF. BS_INTERN gives a stable indirection address even for names
not yet declared (forward refs) — aether[symtab_addr] resolves to the real
lux once the declaration runs, exactly like LOAD_INTERN's backfill trick.
Applied by LOAD_APPLY_LINKS after Wave 1, once every name's address AND
lux-kind prefix (1=Data, ITO_SIZE=ITO) are known — see that routine.//
NOLINK
RVOCA LOAD_CMD_LINK_IMPL  BS_READ_TOKEN
RVOCA LOAD_CL_SI           BS_INTERN
ITO LOAD_CL_SRC            Move  El1=RA_BS_RESULT  Exit=SK_TMP
RVOCA LOAD_CL_R            BS_READ_TOKEN
RVOCA LOAD_CL_RI           BS_INTERN
ITO LOAD_CL_REL            Move  El1=RA_BS_RESULT  Exit=SK_TMP2
RVOCA LOAD_CL_T            BS_READ_TOKEN
RVOCA LOAD_CL_TI           BS_INTERN
ITO LOAD_CL_TGT            Move  El1=RA_BS_RESULT  Exit=SK_TMP3
/Record (src, rel, tgt) symtab addrs into SK_PLINK_BUF
ITO LOAD_CL_W0  Write El1=SK_PLINK_PTR  El2=SK_TMP
FOR C_1 C_2
    ITO LOAD_CL_AF{N} Add   El1=SK_PLINK_PTR  El2={X}       Exit=SK_FLAG
    ITO LOAD_CL_WF{N} Write El1=SK_FLAG       El2={Y}
        C_1 > Y=SK_TMP2
        > Y=SK_TMP3
ITO LOAD_CL_ADV Add   El1=SK_PLINK_PTR  El2=C_3       Exit=SK_PLINK_PTR
ITO LOAD_CL_SEN Write El1=SK_PLINK_PTR  El2=C_0
RVOCA LOAD_CL_SKLS         BS_SKIP_TO_EOL
RREDI LOAD_CL_RRET

── LOAD_CMD_ITO: ITO name op [El1=x] [El2=y] [Exit=z] ───────
//Alloc ITO_SIZE + 2*lcount + (lcount>0?1:0) luces.
Reuses existing lux if name already known. Autolinks from RA_MC_PREV.//
NOLINK
RVOCA LOAD_CMD_ITO    BS_READ_TOKEN
/Get lcount for this name (prepass result)
RVOCA LOAD_CI_LCGET   LOAD_LCOUNT_GET
ITO LOAD_CI_LCSAVE    Move  El1=SK_TMP         Exit=SK_ITO_LCNT
/Record this name's lux-kind prefix (ITO_SIZE) for later LINK application —
/only if it has an lcount entry (lcount>0); a name never LINK'd needs none.
JZ LOAD_CI_PFCK SK_LCG_PFADDR LOAD_CI_NINT
ITO LOAD_CI_PFW       Write El1=SK_LCG_PFADDR  El2=ITO_SIZE
/Intern name → RA_BS_RESULT
RVOCA LOAD_CI_NINT    BS_INTERN
ITO LOAD_CI_NADDR     Move  El1=RA_BS_RESULT   Exit=SK_TMP
/Read word(name_lux): 0 = new, non-zero = existing ITO addr
ITO LOAD_CI_EWGET     Read  El1=SK_TMP         Exit=SK_ITO_ADDR
JZ LOAD_CI_EWCK SK_ITO_ADDR LOAD_CI_OPTOK_NEW
/Existing: read op, skip alloc
RVOCA LOAD_CI_OPTOK   BS_READ_TOKEN
RVOCA LOAD_CI_OPINT   BS_INTERN
CLEAR LOAD_CI_EXCLR   SK_ITO_NEW
ITO LOAD_CI_EXJMP     Jump  Exit=LOAD_CI_SKIP_ALLOC
/New: read op then compute alloc size
NOLINK
RVOCA LOAD_CI_OPTOK_NEW  BS_READ_TOKEN
RVOCA LOAD_CI_OPINTN     BS_INTERN
/Check for __LT_ALLOC_ITO sentinel
JEQ LOAD_CI_ALTCK RA_BS_RESULT SK_ALLOC_ITO_OP LOAD_CI_ALTOP
/Alloc size: ITO_SIZE + 2*lcount + (lcount>0?1:0)
ITO LOAD_CI_SZ1       Mul   El1=SK_ITO_LCNT    El2=C_2              Exit=SK_FLAG
ITO LOAD_CI_SZ2       Add   El1=ITO_SIZE       El2=SK_FLAG           Exit=RA_ALLOC_COUNT
JZ LOAD_CI_TCKZ SK_ITO_LCNT LOAD_CI_ALLOC
ITO LOAD_CI_TADD      Add   El1=RA_ALLOC_COUNT El2=C_1              Exit=RA_ALLOC_COUNT
ITO LOAD_CI_TJMP      Jump  Exit=LOAD_CI_ALLOC
NOLINK
/Sentinel path: wire as Voca SR_GLR RA_LINK
CHAIN LOAD_CI_ALTOP
    Move  El1=Voca      Exit=RA_BS_RESULT
    Move  El1=SR_GLR    Exit=SK_ITO_E1
    Move  El1=RA_LINK   Exit=SK_ITO_EXIT
    Jump  Exit=LOAD_CI_ALLOC
NOLINK
RVOCA LOAD_CI_ALLOC   ALLOC_LUCES
ITO LOAD_CI_SAVEADDR  Move  El1=RA_ALLOC_RESULT Exit=SK_ITO_ADDR
/Register name → ITO
ITO LOAD_CI_REGN      Write El1=SK_TMP          El2=SK_ITO_ADDR
/Self-ref
ITO LOAD_CI_SELF      Write El1=SK_ITO_ADDR    El2=SK_ITO_ADDR
ITO LOAD_CI_NEWSET    Move  El1=C_1            Exit=SK_ITO_NEW
ITO LOAD_CI_NEWJMP    Jump  Exit=LOAD_CI_SKIP_ALLOC
/Write op, parse els
NOLINK
ITO LOAD_CI_SKIP_ALLOC  Add  El1=SK_ITO_ADDR   El2=C_1             Exit=SK_FLAG
ITO LOAD_CI_OPW         Write El1=SK_FLAG        El2=RA_BS_RESULT
CLEAR LOAD_CI_CE1 SK_ITO_E1
CLEAR LOAD_CI_CE2 SK_ITO_E2
CLEAR LOAD_CI_CEX SK_ITO_EXIT
/Read key=value els using unified LOAD_ITO_READ_ARGS
RVOCA LOAD_CI_ARGS    LOAD_ITO_READ_ARGS
/Write e1/e2/exit (slots 2,3,4)
FOR C_2 C_3 C_4
    ITO LOAD_CI_S{N}       Add   El1=SK_ITO_ADDR   El2={X}             Exit=SK_FLAG
    ITO LOAD_CI_W{N}       Write El1=SK_FLAG        El2={Y}
        C_2 > Y=SK_ITO_E1
        C_3 > Y=SK_ITO_E2
        > Y=SK_ITO_EXIT
/Autolink: only for new allocs
JZ LOAD_CI_NEWCK SK_ITO_NEW LOAD_CI_RET
ITO LOAD_CI_LP        Move  El1=RA_MC_PREV    Exit=RA_NL_PREV
ITO LOAD_CI_LN        Move  El1=SK_ITO_ADDR   Exit=RA_NL_NEXT
RVOCA LOAD_CI_LNK     LINK_NEXT
ITO LOAD_CI_NOAUTO    Move  El1=SK_ITO_ADDR   Exit=RA_MC_PREV
RREDI LOAD_CI_RET

── LOAD_ITO_READ_ARGS: read El1=/El2=/Exit= key=value tokens ─
//Saves key bytes B1/B2 BEFORE stripping, routes by key.
Sets SK_ITO_E1, SK_ITO_E2, SK_ITO_EXIT.//
NOLINK
NEWREF LOAD_ITO_READ_ARGS LOAD_IRA_LOOP
JEQ LOAD_IRA_LOOP RA_LOAD_BYTE LF LOAD_IRA_DONE
JZ LOAD_IRA_EOFCK RA_LOAD_BYTE LOAD_IRA_DONE
RVOCA LOAD_IRA_TOK   BS_READ_TOKEN
JZ LOAD_IRA_EMCK RA_LOAD_TLEN LOAD_IRA_DONE
/Save key bytes before BS_TOKEN_VALUE strips them
ITO LOAD_IRA_KB1A    Add   El1=BS_TOKBUF_BASE El2=C_1      Exit=SK_TMP
ITO LOAD_IRA_KB1     Read  El1=SK_TMP         Exit=SK_IRA_KEY1
ITO LOAD_IRA_KB2A    Add   El1=BS_TOKBUF_BASE El2=C_2      Exit=SK_TMP
ITO LOAD_IRA_KB2     Read  El1=SK_TMP         Exit=SK_IRA_KEY2
/Strip key=, intern value
RVOCA LOAD_IRA_KV    BS_TOKEN_VALUE
RVOCA LOAD_IRA_VI    BS_INTERN
/Route by B1: l(108)→El*, x(120)→Exit
JEQ LOAD_IRA_CKL SK_IRA_KEY1 ASCII_ll LOAD_IRA_EL
JEQ LOAD_IRA_CKX SK_IRA_KEY1 ASCII_xl LOAD_IRA_EXIT
ITO LOAD_IRA_CONT    Jump  Exit=LOAD_IRA_LOOP
NOLINK
/El: B2='1' → E1, else → E2
JEQ LOAD_IRA_EL SK_IRA_KEY2 ASCII_1 LOAD_IRA_E1
ITO LOAD_IRA_WE2     Move  El1=RA_BS_RESULT  Exit=SK_ITO_E2
ITO LOAD_IRA_CE2     Jump  Exit=LOAD_IRA_LOOP
NOLINK
ITO LOAD_IRA_E1      Move  El1=RA_BS_RESULT  Exit=SK_ITO_E1
ITO LOAD_IRA_CE1     Jump  Exit=LOAD_IRA_LOOP
NOLINK
ITO LOAD_IRA_EXIT    Move  El1=RA_BS_RESULT  Exit=SK_ITO_EXIT
ITO LOAD_IRA_CEX     Jump  Exit=LOAD_IRA_LOOP
NOLINK
RREDI LOAD_IRA_DONE

── LOAD_CMD_BLOCK: BLOCK name count ─────────────────────────
NOLINK
RVOCA LOAD_CMD_BLOCK  BS_READ_TOKEN
RVOCA LOAD_CB_NINT    BS_INTERN
ITO LOAD_CB_NADDR     Move  El1=RA_BS_RESULT  Exit=SK_TMP
JZ LOAD_CB_EXCK RA_BS_RESULT LOAD_CB_DONEW
ITO LOAD_CB_EXCRD     Read  El1=RA_BS_RESULT  Exit=SK_FLAG
JZ LOAD_CB_EXCZ SK_FLAG LOAD_CB_SKLS
//Explicit redirect: NOLINK below intentionally blocks auto-chaining INTO
LOAD_CB_DONEW (it's also JZ LOAD_CB_EXCK's own explicit jump target above,
and must not be silently fallen-into from whatever else precedes it in
the file). But that same NOLINK means JZ LOAD_CB_EXCZ's own fallthrough
(SK_FLAG != 0) has no auto-linked destination — without this Jump, it
would default to "pc + ITO_SIZE", landing in whatever unrelated code
happens to be allocated immediately afterward.//
ITO LOAD_CB_EXFALL    Jump  Exit=LOAD_CB_DONEW
NOLINK
RVOCA LOAD_CB_DONEW   BS_READ_TOKEN
RVOCA LOAD_CB_CPINT   BS_PARSE_INT
ITO LOAD_CB_MUL       Mul   El1=RA_BS_PIVAL   El2=C_2              Exit=RA_ALLOC_COUNT
RVOCA LOAD_CB_ALLOC   ALLOC_LUCES
ITO LOAD_CB_REG       Write El1=SK_TMP         El2=RA_ALLOC_RESULT
RVOCA LOAD_CB_SKLS    BS_SKIP_TO_EOL
RREDI LOAD_CB_RRET

── LOAD_CMD_NEWREF: NEWREF name [ref] ────────────────────────
NOLINK
RVOCA LOAD_CMD_NEWREF  BS_READ_TOKEN
CLEAR LOAD_CNR_SETL   SK_TMP
RVOCA LOAD_CNR_ICTX   LOAD_CN_INTERN
ITO LOAD_CNR_NADDR    Move  El1=RA_BS_RESULT  Exit=RA_BS_EL0
RVOCA LOAD_CNR_RTOK   BS_READ_TOKEN
JZ LOAD_CNR_RLEN RA_LOAD_TLEN LOAD_CNR_SELF
RVOCA LOAD_CNR_RLK    BS_LOOKUP
ITO LOAD_CNR_WR       Write El1=RA_BS_EL0    El2=RA_BS_RESULT
RVOCA LOAD_CNR_SKPL   BS_SKIP_TO_EOL
RREDI LOAD_CNR_RRET
NOLINK
ITO LOAD_CNR_SELF     Write El1=RA_BS_EL0    El2=RA_BS_EL0
RVOCA LOAD_CNR_SRSK   BS_SKIP_TO_EOL
RREDI LOAD_CNR_SRRET

── LOAD_CMD_NEWSET: NEWSET name value ────────────────────────
//Sets word(name) = integer value or addr(symbol).
Adds lumen (Constant → Yaku) for compile-time constant detection.//
NOLINK
RVOCA LOAD_CMD_NEWSET  BS_READ_TOKEN
RVOCA LOAD_CNS_NLK    BS_LOOKUP
ITO LOAD_CNS_NADDR    Move  El1=RA_BS_RESULT  Exit=RA_BS_EL0
RVOCA LOAD_CNS_VTOK   BS_READ_TOKEN
RVOCA LOAD_CNS_PI     BS_PARSE_INT
ITO LOAD_CNS_WR       Write El1=RA_BS_EL0    El2=RA_BS_PIVAL
/Tag as Constant
CHAIN LOAD_CNS_LSRC
    Move  El1=RA_BS_EL0    Exit=RA_LM_SRC
    Move  El1=Constant     Exit=RA_LM_REL
    Move  El1=Yaku         Exit=RA_LM_EXIT
    Move  El1=C_1          Exit=RA_LM_OFFSET
RVOCA LOAD_CNS_LAL    ADD_LUMEN
RVOCA LOAD_CNS_SKPL   BS_SKIP_TO_EOL
RREDI LOAD_CNS_RRET

── LOAD_CMD_SETREF: SETREF name ref ──────────────────────────
NOLINK
RVOCA LOAD_CMD_SETREF  BS_READ_TOKEN
RVOCA LOAD_CSR_NLK    BS_LOOKUP
ITO LOAD_CSR_NADDR    Move  El1=RA_BS_RESULT  Exit=RA_BS_EL0
RVOCA LOAD_CSR_RTOK   BS_READ_TOKEN
RVOCA LOAD_CSR_RLK    BS_LOOKUP
ITO LOAD_CSR_WR       Write El1=RA_BS_EL0    El2=RA_BS_RESULT
RVOCA LOAD_CSR_SKPL   BS_SKIP_TO_EOL
RREDI LOAD_CSR_RRET

── LOAD_CMD_RVOCA_IMPL: RVOCA name sub ──────────────────────
NOLINK
RVOCA LOAD_CMD_RVOCA_IMPL  BS_READ_TOKEN
RVOCA LOAD_CRV_NLK    BS_LOOKUP
ITO LOAD_CRV_NADDR    Move  El1=RA_BS_RESULT  Exit=RA_BS_EL0
RVOCA LOAD_CRV_STOK   BS_READ_TOKEN
RVOCA LOAD_CRV_SLK    BS_LOOKUP
ITO LOAD_CRV_SADDR    Move  El1=RA_BS_RESULT  Exit=RA_BS_EL1
RVOCA LOAD_CRV_SKP    BS_SKIP_TO_EOL
JZ LOAD_CRV_NCKZ RA_BS_EL0 LOAD_CRV_DONE
ITO LOAD_CRV_SELF     Write El1=RA_BS_EL0    El2=RA_BS_EL0
FOR C_1 C_2 C_4
    ITO LOAD_CRV_S{N}      Add   El1=RA_BS_EL0    El2={X}              Exit=SK_FLAG
    ITO LOAD_CRV_W{N}      Write El1=SK_FLAG       El2={Y}
        C_1 > Y=Voca
        C_2 > Y=RA_BS_EL1
        > Y=RA_LINK_REF
ITO LOAD_CRV_LP       Move  El1=RA_MC_PREV    Exit=RA_NL_PREV
ITO LOAD_CRV_LN       Move  El1=RA_BS_EL0    Exit=RA_NL_NEXT
RVOCA LOAD_CRV_LNK    LINK_NEXT
ITO LOAD_CRV_SETPREV  Move  El1=RA_BS_EL0    Exit=RA_MC_PREV
RREDI LOAD_CRV_DONE

── LOAD_CMD_RREDI_IMPL: RREDI name ──────────────────────────
/Resets RA_MC_PREV=0 — RREDI breaks autolink chain.
NOLINK
RVOCA LOAD_CMD_RREDI_IMPL  BS_READ_TOKEN
RVOCA LOAD_CRR_NLK    BS_LOOKUP
ITO LOAD_CRR_NADDR    Move  El1=RA_BS_RESULT  Exit=RA_BS_EL0
RVOCA LOAD_CRR_SKP    BS_SKIP_TO_EOL
JZ LOAD_CRR_NCKZ RA_BS_EL0 LOAD_CRR_DONE
ITO LOAD_CRR_SELF     Write El1=RA_BS_EL0    El2=RA_BS_EL0
FOR C_1 C_2 C_4
    ITO LOAD_CRR_S{N}      Add   El1=RA_BS_EL0    El2={X}              Exit=SK_FLAG
    ITO LOAD_CRR_W{N}      Write El1=SK_FLAG       El2={Y}
        C_1 > Y=Redi
        > Y=RA_LINK_REF
ITO LOAD_CRR_LP       Move  El1=RA_MC_PREV    Exit=RA_NL_PREV
ITO LOAD_CRR_LN       Move  El1=RA_BS_EL0    Exit=RA_NL_NEXT
RVOCA LOAD_CRR_LNK    LINK_NEXT
CLEAR LOAD_CRR_CLRPREV RA_MC_PREV
RREDI LOAD_CRR_DONE

── LOAD_CMD_NOLINK: suppress next autolink ───────────────────
/Clears RA_MC_PREV so the very next ITO skips autolink.
/BS_LAST_ITO is preserved — the chain continues after the NOLINK block.
NOLINK
CLEAR LOAD_CMD_NOLINK RA_MC_PREV
RVOCA LOAD_NL_SKP    BS_SKIP_TO_EOL
RREDI LOAD_NL_RET

── LOAD_CMD_UNKNOWN: lookup SK_CMD_ADDR → call handler ───────
//SK_CMD_ADDR = addr of cmd lux (set by LOAD_DISPATCH_LINE).
Builder macros (Exit slot == RA_LINK): read MA* from file, call.
Native handlers: call directly with RA_MC_PREV set.
Unknown (word==0): skip rest of line.//
NOLINK
ITO LOAD_CMD_UNKNOWN  Read  El1=SK_CMD_ADDR   Exit=SK_TMP
JZ LOAD_CU_CK SK_TMP LOAD_CU_SKPL
/Detect builder macro: Exit slot (ITO+4) == RA_LINK_REF
ITO LOAD_CU_BME1S    Add   El1=SK_TMP         El2=C_4              Exit=SK_FLAG
ITO LOAD_CU_BME1R    Read  El1=SK_FLAG        Exit=SK_FLAG
JEQ LOAD_CU_BMCK SK_FLAG RA_LINK_REF LOAD_CU_PREV
ITO LOAD_CU_BMBLD    Jump  Exit=LOAD_CU_BUILDER
/Native handler
ITO LOAD_CU_PREV     Move  El1=BS_LAST_ITO   Exit=RA_MC_PREV
ITO LOAD_CU_MARET    Move  El1=RA_MC_PREV    Exit=RA_MA_RET
ITO LOAD_CU_CALL     Voca  El1=SK_TMP        Exit=RA_LINK
ITO LOAD_CU_SYNC     Move  El1=RA_MC_PREV    Exit=BS_LAST_ITO
RREDI LOAD_CU_DRET
/Builder macro: read MA* from file (key=value + positional + integer), call
NOLINK
ITO LOAD_CU_BUILDER  Move  El1=SK_TMP        Exit=SK_TMP2
RVOCA LOAD_CU_MAS    LOAD_READARG_KV
//CHAIN: MA0=0 is a valid, meaningful value (manual/anonymous naming
mode), not "no arguments given, skip this line" -- the generic
zero-check below is wrong for CHAIN specifically, since it was
written assuming every builder macro requires at least one argument.
Without this, a bare "CHAIN" line was silently skipped in its
entirety (including never reading its indented body via
LOAD_READ_BODY), leaving the body's first line to be misread as a
top-level command by the next iteration of the outer dispatch loop.//
JEQ LOAD_CU_CHCK     SK_TMP2 CHAIN_START LOAD_CU_BPREV
JZ LOAD_CU_MA0CK RA_MA0 LOAD_CU_SKPL
/Auto-complete _J/_K for JEQ and JZ macros
JEQ LOAD_CU_JZCK SK_TMP2 JZ LOAD_CU_JZ_PATH
JEQ LOAD_CU_JEQCK SK_TMP2 JEQ LOAD_CU_JEQ_PATH
ITO LOAD_CU_JK_SKIP  Jump  Exit=LOAD_CU_BPREV
NOLINK
RVOCA LOAD_CU_JEQ_PATH  LOAD_MA_JEQ_J
ITO LOAD_CU_JK_DONE  Jump  Exit=LOAD_CU_BPREV
NOLINK
RVOCA LOAD_CU_JZ_PATH   LOAD_MA_JZ_J
ITO LOAD_CU_BPREV    Move  El1=BS_LAST_ITO  Exit=RA_MC_PREV
ITO LOAD_CU_BMAR     Move  El1=RA_MC_PREV   Exit=RA_MA_RET
ITO LOAD_CU_BCALL    Voca  El1=SK_TMP2      Exit=RA_LINK
ITO LOAD_CU_BSYNC    Move  El1=RA_MA_RET   Exit=BS_LAST_ITO
RREDI LOAD_CU_BRET
NOLINK
RVOCA LOAD_CU_SKPL   BS_SKIP_TO_EOL
RREDI LOAD_CU_SKRRET

── LOAD_READARG_KV: unified arg reader ───────────────────────
//Single function for reading macro arguments from file.
Handles: key=value (El1=/El2=/Exit=), integer literals → C_N addr,
and positional tokens → MA0..MA7 in order.
Replaces both BS_READ_ARGS (positional-only) and the per-macro inline
key parsers. Uses BS_SCAN_EQ (separate, composable) to detect '='.
Sets MA0..MA7. Stops at LF/EOF/comment.//
NOLINK
ITO LOAD_READARG_KV  Move  El1=C_1          Exit=SK_POS_SLOT
/Clear MA0..MA7
FOR RA_MA0 RA_MA1 RA_MA2 RA_MA3 RA_MA4 RA_MA5 RA_MA6 RA_MA7
    CLEAR LOAD_RKVA_CLR{N} {X}
ITO LOAD_RKVA_LOOPJ  Jump  Exit=LOAD_RKVA_LOOP
NOLINK
JEQ LOAD_RKVA_LOOP RA_LOAD_BYTE LF LOAD_RKVA_DONE
JZ LOAD_RKVA_EOFCK RA_LOAD_BYTE LOAD_RKVA_DONE
RVOCA LOAD_RKVA_TOK  BS_READ_TOKEN
JZ LOAD_RKVA_EMCK RA_LOAD_TLEN LOAD_RKVA_DONE
/Use BS_SCAN_EQ (separate, composable) to detect key=value
RVOCA LOAD_RKVA_SCAN BS_SCAN_EQ
JZ LOAD_RKVA_EQCK SK_FLAG LOAD_RKVA_POS
/Has '=': check first byte for key type
ITO LOAD_RKVA_KB0    Read  El1=BS_TOKBUF_BASE Exit=SK_TMP
JEQ LOAD_RKVA_KE SK_TMP ASCII_E LOAD_RKVA_EKEY
ITO LOAD_RKVA_KUNK   Jump  Exit=LOAD_RKVA_POS
NOLINK
ITO LOAD_RKVA_EKEY   Add   El1=BS_TOKBUF_BASE El2=C_1             Exit=SK_TMP
ITO LOAD_RKVA_KB1    Read  El1=SK_TMP         Exit=SK_TMP
JEQ LOAD_RKVA_EL SK_TMP ASCII_ll LOAD_RKVA_EL
JEQ LOAD_RKVA_EX SK_TMP ASCII_xl LOAD_RKVA_EXIT
ITO LOAD_RKVA_EUNK   Jump  Exit=LOAD_RKVA_POS
NOLINK
ITO LOAD_RKVA_EL     Add   El1=BS_TOKBUF_BASE El2=C_2             Exit=SK_TMP
ITO LOAD_RKVA_ELD    Read  El1=SK_TMP         Exit=SK_TMP
RVOCA LOAD_RKVA_ELV  BS_TOKEN_VALUE
/Integer or symbol?
RVOCA LOAD_RKVA_ELRD LOAD_RKVA_RESOLVE
JEQ LOAD_RKVA_EL1 SK_TMP ASCII_1 LOAD_RKVA_SET2
ITO LOAD_RKVA_SET3   Move  El1=SK_MA_RESULT  Exit=RA_MA3
ITO LOAD_RKVA_C3     Jump  Exit=LOAD_RKVA_LOOP
NOLINK
ITO LOAD_RKVA_SET2   Move  El1=SK_MA_RESULT  Exit=RA_MA2
ITO LOAD_RKVA_C2     Jump  Exit=LOAD_RKVA_LOOP
NOLINK
NEWREF LOAD_RKVA_EXIT LOAD_RKVA_EXITV
RVOCA LOAD_RKVA_EXITV BS_TOKEN_VALUE
RVOCA LOAD_RKVA_EXRD  LOAD_RKVA_RESOLVE
ITO LOAD_RKVA_SET4   Move  El1=SK_MA_RESULT  Exit=RA_MA4
ITO LOAD_RKVA_C4J    Jump  Exit=LOAD_RKVA_LOOP
/Positional: resolve then write to MA[pos_slot]
NOLINK
RVOCA LOAD_RKVA_POS  LOAD_RKVA_RESOLVE
SWITCH SK_POS_SLOT
    1 LOAD_RKVA_W0
    2 LOAD_RKVA_WP0
    3 LOAD_RKVA_WP1
    4 LOAD_RKVA_WP2
    5 LOAD_RKVA_WP3
    6 LOAD_RKVA_WP4
    7 LOAD_RKVA_WP5
    8 LOAD_RKVA_WP6
ITO LOAD_RKVA_FALL  Jump  Exit=LOAD_RKVA_LOOP
/Positional slot 0 (MA0): write and save hash for _J/_K extension
NOLINK
ITO LOAD_RKVA_W0     Move  El1=SK_MA_RESULT   Exit=RA_MA0
ITO LOAD_RKVA_W0_SH  Move  El1=SK_MA_HASH     Exit=SK_MA_HASH0
ITO LOAD_RKVA_INC0   Add   El1=SK_POS_SLOT    El2=C_1  Exit=SK_POS_SLOT
ITO LOAD_RKVA_J0     Jump  Exit=LOAD_RKVA_LOOP
FOR RA_MA1 RA_MA2 RA_MA3 RA_MA4 RA_MA5 RA_MA6 RA_MA7
    NOLINK
    ITO LOAD_RKVA_WP{N}    Move  El1=SK_MA_RESULT   Exit={X}
    ITO LOAD_RKVA_INCP{N}  Add   El1=SK_POS_SLOT    El2=C_1  Exit=SK_POS_SLOT
    ITO LOAD_RKVA_JP{N}    Jump  Exit=LOAD_RKVA_LOOP
NOLINK
RREDI LOAD_RKVA_DONE

── LOAD_RKVA_RESOLVE: resolve current tokbuf → SK_MA_RESULT ──
//Handles: integer ('-' or digit) → C_N addr, symbol → BS_INTERN result.
Sets SK_MA_RESULT and SK_MA_HASH.//
NOLINK
ITO LOAD_RKVA_RESOLVE  Read  El1=BS_TOKBUF_BASE Exit=SK_MA_RESULT
/Integer?
ITO LOAD_RKVR_DIGS   Sub   El1=SK_MA_RESULT  El2=ASCII_0           Exit=SK_TMP
ITO LOAD_RKVR_DIGCK  ULess El1=SK_TMP        El2=C_10              Exit=SK_FLAG
ITO LOAD_RKVR_DIGCJ  JumpIf El1=SK_FLAG      Exit=LOAD_RKVR_INT
JEQ LOAD_RKVR_NEGCK SK_MA_RESULT MINUS LOAD_RKVR_INT
/Symbol
RVOCA LOAD_RKVR_LK   BS_LOOKUP
ITO LOAD_RKVR_LKRES  Move  El1=RA_BS_RESULT Exit=SK_MA_RESULT
ITO LOAD_RKVR_SVHASH Move  El1=RA_LOAD_HASH Exit=SK_MA_HASH
RREDI LOAD_RKVR_RRET
/Integer → C_N addr: C_N lux = addr(C_0) + value*2
NOLINK
RVOCA LOAD_RKVR_INT  BS_PARSE_INT
ITO LOAD_RKVR_C2     Mul   El1=RA_BS_PIVAL  El2=C_2               Exit=SK_TMP
ITO LOAD_RKVR_CN     Add   El1=RA_C0_REF    El2=SK_TMP             Exit=SK_MA_RESULT
ITO LOAD_RKVR_SVHI   Move  El1=RA_LOAD_HASH Exit=SK_MA_HASH
RREDI LOAD_RKVR_INTRR

── LOAD_MA_JEQ_J: auto-complete MA4 (_J) for JEQ ────────────
//JEQ macro no longer needs a _K suffix (NOP removed — JumpIf's own
slot 5 serves as the fall-through point). Only _J needs resolving.//
NOLINK
JZ LOAD_MA_JEQ_J RA_MA4 LOAD_MAJ_DO_J
RREDI LOAD_MAJ_RRET
CHAIN
    LOAD_MAJ_DO_J   Mul   El1=SK_MA_HASH0  El2=C_33              Exit=RA_BS_TMP2
    LOAD_MAJ_JU1    Add   El1=RA_BS_TMP2  El2=UNDERSCORE         Exit=RA_BS_TMP2
    LOAD_MAJ_JJ1    Mul   El1=RA_BS_TMP2  El2=C_33               Exit=RA_LOAD_HASH
    LOAD_MAJ_JJ2    Add   El1=RA_LOAD_HASH El2=ASCII_J           Exit=RA_LOAD_HASH
        RVOCA LOAD_MAJ_JLK  BS_LOOKUP
    LOAD_MAJ_JSET   Move  El1=RA_BS_RESULT Exit=RA_MA4
        JZ LOAD_MAJ_JCK RA_MA4 LOAD_MAJ_JALLOC
    LOAD_MAJ_JFOUND Jump  Exit=LOAD_MAJ_DONE
NOLINK
ALLOC_TO LOAD_MAJ_JALLOC RA_MA4 ITO_SIZE
RREDI LOAD_MAJ_DONE

── LOAD_MA_JZ_J: auto-complete MA3 (_J) for JZ ──────────────
/JZ macro no longer needs a _K suffix (same reasoning as JEQ above).
NOLINK
JZ LOAD_MA_JZ_J RA_MA3 LOAD_MZJ_JLK_GO
RREDI LOAD_MZJ_DONE
NOLINK
ITO LOAD_MZJ_JLK_GO Mul  El1=SK_MA_HASH0  El2=C_33              Exit=RA_LOAD_HASH
ITO LOAD_MZJ_JU1    Add  El1=RA_LOAD_HASH El2=UNDERSCORE         Exit=RA_LOAD_HASH
ITO LOAD_MZJ_JK1    Mul  El1=RA_LOAD_HASH El2=C_33               Exit=RA_LOAD_HASH
ITO LOAD_MZJ_JK2    Add  El1=RA_LOAD_HASH El2=ASCII_J            Exit=RA_LOAD_HASH
RVOCA LOAD_MZJ_JLK  BS_LOOKUP
ITO LOAD_MZJ_JSET   Move  El1=RA_BS_RESULT Exit=RA_MA3
JZ LOAD_MZJ_JFCK RA_MA3 LOAD_MZJ_JALLOC
RREDI LOAD_MZJ_DONE2
NOLINK
ALLOC_TO LOAD_MZJ_JALLOC RA_MA3 ITO_SIZE
RREDI LOAD_MZJ_DONE3

── LOAD_PREPASS_FILE: scan file, count LINK lumina per src ───
NOLINK
RVOCA LOAD_PREPASS_FILE  LOAD_OPEN_FILE
ITO LOAD_PP_JMP2LP   Jump  Exit=LOAD_PP_LOOP
NOLINK
RVOCA LOAD_PP_LOOP   BS_READ_BYTE
JZ LOAD_PP_EOFCK RA_LOAD_BYTE LOAD_PP_CLOSE
/LF: empty line
JEQ LOAD_PP_LFCK RA_LOAD_BYTE LF LOAD_PP_LOOP
/Comment
JEQ LOAD_PP_SLCK RA_LOAD_BYTE SLASH LOAD_PP_SL2
/Indented: skip
JEQ LOAD_PP_SPCK RA_LOAD_BYTE SP LOAD_PP_SKIP
JEQ LOAD_PP_TBCK RA_LOAD_BYTE TAB LOAD_PP_SKIP
/Prepend peeked byte, collect rest of token
ITO LOAD_PP_SVCB     Move  El1=RA_LOAD_BYTE  Exit=SK_TMP
ITO LOAD_PP_TB0      Move  El1=BS_TOKBUF_BASE Exit=RA_BS_TMP
ITO LOAD_PP_TBW      Write El1=RA_BS_TMP      El2=SK_TMP
ITO LOAD_PP_TINI     Move  El1=C_1            Exit=RA_LOAD_TLEN
ITO LOAD_PP_H0       Mul   El1=BS_HASH0       El2=C_33             Exit=RA_LOAD_HASH
ITO LOAD_PP_H1       Add   El1=RA_LOAD_HASH   El2=SK_TMP           Exit=RA_LOAD_HASH
RVOCA LOAD_PP_TOK    LOAD_DL_COLLECT
/Check tokbuf[0]='L', tokbuf[1]='I' → LINK
ITO LOAD_PP_B0       Read  El1=BS_TOKBUF_BASE Exit=SK_TMP
JEQ LOAD_PP_LCK SK_TMP ASCII_L LOAD_PP_CHK2
ITO LOAD_PP_NOTL     Jump  Exit=LOAD_PP_SKIP
NOLINK
ITO LOAD_PP_CHK2     Add   El1=BS_TOKBUF_BASE El2=C_1             Exit=SK_TMP2
ITO LOAD_PP_B1       Read  El1=SK_TMP2        Exit=SK_TMP
JEQ LOAD_PP_ICK SK_TMP ASCII_I LOAD_PP_LINK
ITO LOAD_PP_NOTL2    Jump  Exit=LOAD_PP_SKIP
/LINK found: read src token, increment its lcount
NOLINK
RVOCA LOAD_PP_LINK   BS_READ_TOKEN
RVOCA LOAD_PP_LINKI  BS_INTERN
ITO LOAD_PP_LHK      Move  El1=RA_BS_RESULT  Exit=RA_LOAD_HASH
RVOCA LOAD_PP_INC    LOAD_LCOUNT_INC
ITO LOAD_PP_IJMP     Jump  Exit=LOAD_PP_SKIP
NOLINK
//Calls BS_SKIP_TO_EOL directly — that routine is itself now a safe no-op
when already at LF/EOF (see lexer.re), so no separate guard is needed here.//
RVOCA LOAD_PP_SKIP   BS_SKIP_TO_EOL
ITO LOAD_PP_SJMP     Jump  Exit=LOAD_PP_LOOP
/Comment: double-slash block or single-line
NOLINK
RVOCA LOAD_PP_SL2    BS_READ_BYTE
JEQ LOAD_PP_SL2CK RA_LOAD_BYTE SLASH LOAD_PP_BLOCK
RVOCA LOAD_PP_SL2SK  BS_SKIP_TO_EOL
ITO LOAD_PP_SL2JMP   Jump  Exit=LOAD_PP_LOOP
NOLINK
RVOCA LOAD_PP_BLOCK  BS_READ_BYTE
JZ LOAD_PP_BEOFCK RA_LOAD_BYTE LOAD_PP_CLOSE
JEQ LOAD_PP_BSLCK RA_LOAD_BYTE SLASH LOAD_PP_BSL2
ITO LOAD_PP_BJMP     Jump  Exit=LOAD_PP_BLOCK
NOLINK
RVOCA LOAD_PP_BSL2   BS_READ_BYTE
JZ LOAD_PP_BSL2EOF RA_LOAD_BYTE LOAD_PP_CLOSE
JEQ LOAD_PP_BSL2CK RA_LOAD_BYTE SLASH LOAD_PP_BEND
ITO LOAD_PP_BSL2NKJ  JumpIf El1=SK_FLAG      Exit=LOAD_PP_BSL2
ITO LOAD_PP_BSL2NJP  Jump  Exit=LOAD_PP_BLOCK
NOLINK
RVOCA LOAD_PP_BEND   BS_SKIP_TO_EOL
ITO LOAD_PP_BJMP2    Jump  Exit=LOAD_PP_LOOP
NOLINK
ITO LOAD_PP_CLOSE    Move  El1=SYS_CLOSE    Exit=SC_NR
ITO LOAD_PP_CLFD     Move  El1=RA_LOAD_FD   Exit=SC_A0
ITO LOAD_PP_CLXR     Exire El1=C_0          El2=C_0              Exit=C_0
RREDI LOAD_PP_RRET

── LOAD_LCOUNT_INC: increment LINK count for src name ────────
//IN: RA_LOAD_HASH = intern addr of src name.
Entry format: [hash, count, prefix, hash, count, prefix, ..., 0].
prefix starts at 0 (unknown); LOAD_CMD_ITO/LOAD_CMD_NEW fill it in during
Wave 1 once the name's declaration (and thus its lux kind) is known.//
NOLINK
ITO LOAD_LCOUNT_INC  Move  El1=SK_LCOUNT_BASE Exit=SK_TMP
ITO LOAD_LCI_LOOP    Read  El1=SK_TMP          Exit=SK_TMP2
JZ LOAD_LCI_ZCKL SK_TMP2 LOAD_LCI_NEW
JEQ LOAD_LCI_MCK SK_TMP2 RA_LOAD_HASH LOAD_LCI_HIT
ITO LOAD_LCI_ADV     Add   El1=SK_TMP           El2=C_3            Exit=SK_TMP
ITO LOAD_LCI_JMP     Jump  Exit=LOAD_LCI_LOOP
NOLINK
ITO LOAD_LCI_HIT     Add   El1=SK_TMP           El2=C_1            Exit=SK_TMP2
ITO LOAD_LCI_RD      Read  El1=SK_TMP2          Exit=SK_FLAG
ITO LOAD_LCI_INC     Add   El1=SK_FLAG          El2=C_1            Exit=SK_FLAG
ITO LOAD_LCI_WR      Write El1=SK_TMP2          El2=SK_FLAG
RREDI LOAD_LCI_RRET
CHAIN
    LOAD_LCI_NEW  Write El1=SK_LCOUNT_PTR El2=RA_LOAD_HASH
    LOAD_LCI_INC2 Add   El1=SK_LCOUNT_PTR El2=C_1                 Exit=SK_TMP2
    LOAD_LCI_WC   Write El1=SK_TMP2       El2=C_1
    LOAD_LCI_PFA  Add   El1=SK_LCOUNT_PTR El2=C_2                 Exit=SK_TMP2
    LOAD_LCI_WPF  Write El1=SK_TMP2       El2=C_0
    LOAD_LCI_ADV2 Add   El1=SK_LCOUNT_PTR El2=C_3                 Exit=SK_LCOUNT_PTR
    LOAD_LCI_SEN  Write El1=SK_LCOUNT_PTR El2=C_0
        RREDI LOAD_LCI_NRRET

── LOAD_LCOUNT_GET: get LINK count for name → SK_TMP ─────────
//OUT: SK_TMP = count (0 if miss). SK_LCG_PFADDR = addr of this entry's
prefix slot (0 if miss) — callers may Write a prefix value there.//
NOLINK
ITO LOAD_LCOUNT_GET  Move  El1=SK_LCOUNT_BASE Exit=SK_TMP2
ITO LOAD_LCG_LOOP    Read  El1=SK_TMP2        Exit=SK_TMP
JZ LOAD_LCG_ZCKL SK_TMP LOAD_LCG_MISS
JEQ LOAD_LCG_MCK SK_TMP RA_LOAD_HASH LOAD_LCG_HIT
ITO LOAD_LCG_ADV     Add   El1=SK_TMP2         El2=C_3            Exit=SK_TMP2
ITO LOAD_LCG_JMP     Jump  Exit=LOAD_LCG_LOOP
NOLINK
ITO LOAD_LCG_HIT     Add   El1=SK_TMP2         El2=C_1            Exit=SK_TMP2
ITO LOAD_LCG_RD      Read  El1=SK_TMP2         Exit=SK_TMP
ITO LOAD_LCG_PFA     Add   El1=SK_TMP2         El2=C_1            Exit=SK_LCG_PFADDR
RREDI LOAD_LCG_RRET
NOLINK
CLEAR LOAD_LCG_MISS SK_TMP
CLEAR LOAD_LCG_MPFZ SK_LCG_PFADDR
RREDI LOAD_LCG_MRRET

── LOAD_INTERN: intern with backfill support ─────────────────
//If found: return addr in RA_BS_RESULT.
If NOT found: record (SK_INTERN_SLOT, hash) in backfill, return 0.
Use BS_INTERN for names that must exist; LOAD_INTERN for forward refs.//
NOLINK
RVOCA LOAD_INTERN    BS_LOOKUP
JZ LOAD_INT_CK RA_BS_RESULT LOAD_INT_MISS
RREDI LOAD_INT_RRET
CHAIN
    LOAD_INT_MISS   Write El1=SK_BACKFILL_PTR  El2=SK_INTERN_SLOT
    LOAD_INT_BFINC  Add   El1=SK_BACKFILL_PTR  El2=C_1           Exit=SK_BACKFILL_PTR
    LOAD_INT_BFHSH  Write El1=SK_BACKFILL_PTR  El2=RA_LOAD_HASH
    LOAD_INT_BFINC2 Add   El1=SK_BACKFILL_PTR  El2=C_1           Exit=SK_BACKFILL_PTR
    LOAD_INT_BFEND  Write El1=SK_BACKFILL_PTR  El2=C_0
        CLEAR LOAD_INT_ZERO RA_BS_RESULT
        RREDI LOAD_INT_MRRET

── LOAD_CN_INTERN: intern for data-lux commands ──────────────
//Finds or allocates a data lux with packed name at lux[1].
Used by LOAD_CMD_NEWREF and similar.//
NOLINK
RVOCA LOAD_CN_INTERN BS_LOOKUP
JZ LOAD_CNI_CK RA_BS_RESULT LOAD_CNI_ALLOC
RREDI LOAD_CNI_DONE
NOLINK
ALLOC_TO LOAD_CNI_ALLOC RA_BS_EL0 C_2
RVOCA LOAD_CNI_PACK  BS_PACK_TOKBUF
ITO LOAD_CNI_RST     Move  El1=RA_BS_EL0   Exit=RA_BS_RESULT
ITO LOAD_CNI_N1A     Add   El1=RA_BS_RESULT El2=C_1             Exit=SK_TMP
ITO LOAD_CNI_N1W     Write El1=SK_TMP        El2=RA_BS_TMP2
CHAIN LOAD_CNI_HH
    Move  El1=RA_LOAD_HASH Exit=RA_HT_HASH
    Move  El1=RA_BS_RESULT Exit=RA_HT_LID
    Move  El1=BS_HT_BASE   Exit=RA_HT_BASE
    Move  El1=BS_HT_MASK   Exit=RA_HT_MASK
    Move  El1=BS_HT_SIZE   Exit=RA_HT_SIZE
        RVOCA LOAD_CNI_HT    HT_INSERT
RREDI LOAD_CNI_RRET

── LOAD_APPLY_BACKFILL: resolve forward references ────────────
NOLINK
ITO LOAD_APPLY_BACKFILL  Move  El1=SK_BACKFILL_BASE Exit=SK_ABF_PTR
ITO LOAD_ABF_LOOP    Read  El1=SK_ABF_PTR    Exit=SK_ABF_SLOT
JZ LOAD_ABF_CK0 SK_ABF_SLOT LOAD_ABF_DONE
ITO LOAD_ABF_HADD    Add   El1=SK_ABF_PTR    El2=C_1             Exit=SK_TMP
ITO LOAD_ABF_HREAD   Read  El1=SK_TMP        Exit=SK_ABF_HASH
CHAIN LOAD_ABF_LHSH
    Move  El1=SK_ABF_HASH   Exit=RA_HT_HASH
    Move  El1=BS_HT_BASE    Exit=RA_HT_BASE
    Move  El1=BS_HT_MASK    Exit=RA_HT_MASK
    Move  El1=BS_HT_SIZE    Exit=RA_HT_SIZE
RVOCA LOAD_ABF_LK    HT_LOOKUP
JZ LOAD_ABF_RCKZ RA_HT_RESULT LOAD_ABF_NEXT
ITO LOAD_ABF_WRITE   Write El1=SK_ABF_SLOT   El2=RA_HT_RESULT
ITO LOAD_ABF_NEXT    Add   El1=SK_ABF_PTR    El2=C_2             Exit=SK_ABF_PTR
ITO LOAD_ABF_JMP     Jump  Exit=LOAD_ABF_LOOP
NOLINK
RREDI LOAD_ABF_DONE

── LOAD_APPLY_LINKS: apply deferred LINK lumina after Wave 1 ──
//By now every name's address (via symtab indirection) and lux-kind prefix
(1=Data, ITO_SIZE=ITO; written by LOAD_CMD_NEW/LOAD_CMD_ITO whenever the
name had >=1 LINK reference in Wave 0) are resolved. Walks SK_PLINK_BUF,
resolves each (src,rel,tgt) triple, looks up src's prefix, and calls
ADD_LUMEN with the correct offset — never guesses, never scans lux content.//
NOLINK
ITO LOAD_APPLY_LINKS Move  El1=SK_PLINK_BASE  Exit=SK_PLINK_ITER
NOLINK
ITO LOAD_APL_LOOP   Read  El1=SK_PLINK_ITER  Exit=SK_TMP        /src symtab addr (0=terminator)
JZ LOAD_APL_CK0 SK_TMP LOAD_APL_DONE
ITO LOAD_APL_RSRC   Read  El1=SK_TMP         Exit=RA_LM_SRC     /resolved src addr
ITO LOAD_APL_HK     Move  El1=SK_TMP         Exit=RA_LOAD_HASH  /lcount key = src symtab addr
RVOCA LOAD_APL_LCG  LOAD_LCOUNT_GET
ITO LOAD_APL_A1     Add   El1=SK_PLINK_ITER  El2=C_1            Exit=SK_TMP2
ITO LOAD_APL_RRELA  Read  El1=SK_TMP2        Exit=SK_TMP2       /rel symtab addr
ITO LOAD_APL_RREL   Read  El1=SK_TMP2        Exit=RA_LM_REL     /resolved rel addr
ITO LOAD_APL_A2     Add   El1=SK_PLINK_ITER  El2=C_2            Exit=SK_TMP3
ITO LOAD_APL_RTGTA  Read  El1=SK_TMP3        Exit=SK_TMP3       /tgt symtab addr
ITO LOAD_APL_RTGT   Read  El1=SK_TMP3        Exit=RA_LM_EXIT    /resolved tgt addr
/Guard: no lcount entry, or prefix never set (declaration missing) → skip, don't corrupt
JZ LOAD_APL_PFCK SK_LCG_PFADDR LOAD_APL_NEXT
ITO LOAD_APL_RPFX   Read  El1=SK_LCG_PFADDR  Exit=RA_LM_OFFSET
JZ LOAD_APL_OFCK RA_LM_OFFSET LOAD_APL_NEXT
RVOCA LOAD_APL_ADD  ADD_LUMEN
NOLINK
ITO LOAD_APL_NEXT   Add   El1=SK_PLINK_ITER  El2=C_3            Exit=SK_PLINK_ITER
ITO LOAD_APL_JMP    Jump  Exit=LOAD_APL_LOOP
NOLINK
RREDI LOAD_APL_DONE

── LOAD_DL_COLLECT: collect remaining bytes of current token ──
//Appends bytes to tokbuf[tlen..], updating hash. Stops at space/LF/EOF/slash.
Does NOT reset tlen or hash. Leaves RA_LOAD_BYTE = terminator.//
NOLINK
NEWREF LOAD_DL_COLLECT LOAD_DL_CO_RD
RVOCA LOAD_DL_CO_RD  BS_READ_BYTE
JZ LOAD_DL_CO_EOFCK RA_LOAD_BYTE LOAD_DL_CO_DONE
JEQ LOAD_DL_CO_LFCK RA_LOAD_BYTE LF LOAD_DL_CO_DONE
JEQ LOAD_DL_CO_SPCK RA_LOAD_BYTE SP LOAD_DL_CO_DONE
JEQ LOAD_DL_CO_TBCK RA_LOAD_BYTE TAB LOAD_DL_CO_DONE
JEQ LOAD_DL_CO_SLCK RA_LOAD_BYTE SLASH LOAD_DL_CO_SL
ITO LOAD_DL_CO_ADDR  Add   El1=BS_TOKBUF_BASE El2=RA_LOAD_TLEN   Exit=SK_TMP
ITO LOAD_DL_CO_WB    Write El1=SK_TMP         El2=RA_LOAD_BYTE
ITO LOAD_DL_CO_HM33  Mul   El1=RA_LOAD_HASH  El2=C_33             Exit=SK_TMP
ITO LOAD_DL_CO_HADD  Add   El1=SK_TMP         El2=RA_LOAD_BYTE    Exit=RA_LOAD_HASH
ITO LOAD_DL_CO_INC   Add   El1=RA_LOAD_TLEN  El2=C_1              Exit=RA_LOAD_TLEN
ITO LOAD_DL_CO_LOOP  Jump  Exit=LOAD_DL_CO_RD
NOLINK
RVOCA LOAD_DL_CO_SL  BS_SKIP_TO_EOL
RREDI LOAD_DL_CO_SLRR
NOLINK
RREDI LOAD_DL_CO_DONE

── LOAD_READ_LINE: stub implementation ───────────────────────
/Reads next line; sets RA_LOAD_BYTE and SK_IND_DEPTH.
NEW SK_IND_DEPTH      /0=none, 1=one level, 2=two+ levels
NEWREF LOAD_READ_LINE LOAD_RL_IMPL
NOLINK
CLEAR LOAD_RL_IMPL SK_IND_DEPTH
RVOCA LOAD_RL_RB     BS_READ_BYTE
JZ LOAD_RL_EOFCK RA_LOAD_BYTE LOAD_RL_DONE
SWITCH RA_LOAD_BYTE
    SP  LOAD_RL_INDENTED
    TAB LOAD_RL_INDENTED
ITO LOAD_RL_UNRD  Sub   El1=RA_LOAD_RPOS  El2=C_1  Exit=RA_LOAD_RPOS
RREDI LOAD_RL_DONE
NOLINK
ITO LOAD_RL_INDENTED Move  El1=C_1          Exit=SK_IND_DEPTH
RVOCA LOAD_RL_RB2    BS_READ_BYTE
JZ LOAD_RL_EOF2 RA_LOAD_BYTE LOAD_RL_INDDONE
SWITCH RA_LOAD_BYTE
    SP  LOAD_RL_INDENTED2
    TAB LOAD_RL_INDENTED2
ITO LOAD_RL_INDDONE2 Jump  Exit=LOAD_RL_INDDONE_UR
NOLINK
ITO LOAD_RL_INDENTED2 Move El1=C_2         Exit=SK_IND_DEPTH
RVOCA LOAD_RL_RB3    BS_READ_BYTE
SWITCH RA_LOAD_BYTE
    SP  LOAD_RL_SKP3
    TAB LOAD_RL_SKP3
ITO LOAD_RL_INDDONE3 Jump  Exit=LOAD_RL_INDDONE_UR
NOLINK
RVOCA LOAD_RL_SKP3   BS_READ_BYTE
SWITCH RA_LOAD_BYTE
    SP  LOAD_RL_SKP3
    TAB LOAD_RL_SKP3
ITO LOAD_RL_SKP3_END Jump  Exit=LOAD_RL_INDDONE_UR
NOLINK
ITO LOAD_RL_EOF2     Jump  Exit=LOAD_RL_INDDONE
NOLINK
ITO LOAD_RL_INDDONE_UR Sub  El1=RA_LOAD_RPOS  El2=C_1  Exit=RA_LOAD_RPOS
RREDI LOAD_RL_INDDONE

── LOAD_READ_TOKEN, LOAD_INTERN_TOKEN: protocol stubs ────────
NEWREF LOAD_READ_TOKEN LOAD_RT_IMPL
NOLINK
RVOCA LOAD_RT_IMPL   BS_READ_TOKEN
RREDI LOAD_RT_RRET

NEWREF LOAD_INTERN_TOKEN LOAD_IT_IMPL
NOLINK
RVOCA LOAD_IT_IMPL   BS_INTERN
ITO LOAD_IT_RES      Move  El1=RA_BS_RESULT Exit=RA_LOAD_RESULT
RREDI LOAD_IT_RRET

── LOAD_READ_BODY: read indented lines into body buffer ───────
//Each line is prefixed with its indent depth byte (1 or 2) as a marker.
Stops at non-indented line or EOF (line NOT consumed).//
NEWREF LOAD_READ_BODY LOAD_RB_IMPL
NOLINK
ITO LOAD_RB_IMPL     Move  El1=SK_BODY_BASE    Exit=SK_BODY_PTR
CLEAR LOAD_RB_LC     SK_BODY_LINES
RVOCA LOAD_RB_RL     LOAD_READ_LINE
JZ LOAD_RB_EOFCK RA_LOAD_BYTE LOAD_RB_DONE
JZ LOAD_RB_INDCK SK_IND_DEPTH LOAD_RB_DONE
JEQ LOAD_RB_OVFLCK SK_BODY_PTR SK_BODY_LIMIT LOAD_RB_RL
ITO LOAD_RB_MARK     Write El1=SK_BODY_PTR    El2=SK_IND_DEPTH
ITO LOAD_RB_MINC     Add   El1=SK_BODY_PTR    El2=C_1             Exit=SK_BODY_PTR
ITO LOAD_RB_STORE    Write El1=SK_BODY_PTR    El2=RA_LOAD_BYTE
ITO LOAD_RB_INC      Add   El1=SK_BODY_PTR    El2=C_1             Exit=SK_BODY_PTR
ITO LOAD_RB_LINC     Add   El1=SK_BODY_LINES  El2=C_1             Exit=SK_BODY_LINES
ITO LOAD_RB_JMP      Jump  Exit=LOAD_RB_RL
NOLINK
RREDI LOAD_RB_DONE

── LOAD_EXPAND_TEMPLATE: expand body for current FOR element ──
//Substitutes {X} → element name, {N} → decimal index of RA_FOR_IDX.
Dispatches each expanded line via LOAD_DISPATCH_LINE.//
NEWREF LOAD_EXPAND_TEMPLATE LOAD_ET_IMPL
NOLINK
ITO LOAD_ET_IMPL     Move  El1=SK_BODY_BASE    Exit=SK_ET_PTR
ITO LOAD_ET_LINE     Move  El1=BS_TOKBUF_BASE  Exit=SK_ET_OUT
CLEAR LOAD_ET_DINIT  SK_ET_DONE
JEQ LOAD_ET_LOOP SK_ET_PTR SK_BODY_PTR LOAD_ET_ENDCK
ITO LOAD_ET_RBYTE    Read  El1=SK_ET_PTR        Exit=SK_TMP
ITO LOAD_ET_INC      Add   El1=SK_ET_PTR        El2=C_1            Exit=SK_ET_PTR
/Skip indent marker bytes (C_1, C_2)
JEQ LOAD_ET_MRK1     SK_TMP C_1 LOAD_ET_LOOP
JEQ LOAD_ET_MRK2     SK_TMP C_2 LOAD_ET_LOOP
/LF → flush line
JEQ LOAD_ET_LFCK SK_TMP LF LOAD_ET_FLUSH_DISPATCH
/'{' → placeholder
JEQ LOAD_ET_LBCK SK_TMP LBRACE LOAD_ET_PLACEHOLDER
/Ordinary byte
WRITE_OUT LOAD_ET_WBYTE SK_TMP
ITO LOAD_ET_JMP      Jump  Exit=LOAD_ET_LOOP
NOLINK
CLEAR LOAD_ET_PLACEHOLDER SK_ET_PH_IDX
ITO LOAD_ET_PH_RD    Read  El1=SK_ET_PTR         Exit=SK_TMP
ITO LOAD_ET_PH_INC   Add   El1=SK_ET_PTR         El2=C_1           Exit=SK_ET_PTR
JEQ LOAD_ET_PH_RBCK SK_TMP RBRACE LOAD_ET_PH_DISPATCH
ITO LOAD_ET_PH_IINC  Add   El1=SK_ET_PH_IDX      El2=C_1           Exit=SK_ET_PH_IDX
ITO LOAD_ET_PH_JMP   Jump  Exit=LOAD_ET_PH_RD
NOLINK
ITO LOAD_ET_PH_DISPATCH Move El1=BS_TOKBUF_BASE Exit=SK_FLAG
ITO LOAD_ET_PH_RB0   Read  El1=SK_FLAG           Exit=SK_TMP
JEQ LOAD_ET_PH_XCK SK_TMP ASCII_X LOAD_ET_EMIT_ELEM
JEQ LOAD_ET_PH_NCK SK_TMP ASCII_N LOAD_ET_EMIT_IDX
/Unknown placeholder: passthrough '{' + content
ITO LOAD_ET_PH_UNK   Move  El1=LBRACE             Exit=SK_TMP
WRITE_OUT LOAD_ET_PH_UW SK_TMP
ITO LOAD_ET_PH_UJMP  Jump  Exit=LOAD_ET_LOOP
NOLINK
/Emit element name bytes from FOR_IRIS buffers
ITO LOAD_ET_EMIT_ELEM Move  El1=RA_FOR_IDX          Exit=SK_TMP
ITO LOAD_ET_EE_IDXA   Add   El1=RA_FOR_IRIS_IDX_BASE El2=SK_TMP     Exit=SK_TMP2
ITO LOAD_ET_EE_OFFSET Read  El1=SK_TMP2              Exit=SK_TMP
ITO LOAD_ET_EE_NBASE  Add   El1=RA_FOR_IRIS_BUF      El2=SK_TMP     Exit=SK_TMP2
ITO LOAD_ET_EE_LOOP   Read  El1=SK_TMP2              Exit=SK_TMP
JZ LOAD_ET_EE_NULCK SK_TMP LOAD_ET_EE_DONE
WRITE_OUT LOAD_ET_EE_WRITE SK_TMP
ITO LOAD_ET_EE_NI     Add   El1=SK_TMP2              El2=C_1         Exit=SK_TMP2
ITO LOAD_ET_EE_JMP2   Jump  Exit=LOAD_ET_EE_LOOP
NOLINK
ITO LOAD_ET_EE_DONE   Jump  Exit=LOAD_ET_LOOP
NOLINK
/Emit index as decimal
ITO LOAD_ET_EMIT_IDX  Move  El1=RA_FOR_IDX            Exit=SK_TMP
RVOCA LOAD_ET_EI_INT  LOAD_EMIT_INT_BYTES
ITO LOAD_ET_EI_JMP    Jump  Exit=LOAD_ET_LOOP
//End of body reached. Flush whatever's pending exactly once — if we land
here again (SK_ET_DONE already 1), the body was already fully flushed and
nothing changed since (SK_ET_PTR/SK_BODY_PTR never advance past this
point), so looping into FLUSH_DISPATCH again would just re-dispatch an
empty line forever.//
NOLINK
JZ LOAD_ET_ENDCK SK_ET_DONE LOAD_ET_EDOFL
RREDI LOAD_ET_DRET
NOLINK
ITO LOAD_ET_EDOFL    Move  El1=C_1  Exit=SK_ET_DONE
ITO LOAD_ET_EDJMP    Jump  Exit=LOAD_ET_FLUSH_DISPATCH
NOLINK
ITO LOAD_ET_FLUSH_DISPATCH  Write El1=SK_ET_OUT  El2=LF
ITO LOAD_ET_FD_INC   Add   El1=SK_ET_OUT          El2=C_1            Exit=SK_ET_OUT
ITO LOAD_ET_FD_FB    Read  El1=BS_TOKBUF_BASE      Exit=RA_LOAD_BYTE
ITO LOAD_ET_FD_LEN   Sub   El1=SK_ET_OUT           El2=BS_TOKBUF_BASE Exit=RA_LOAD_RLEN
CLEAR LOAD_ET_FD_RST RA_LOAD_RPOS
/Copy expanded line to BS_READBUF_BASE so BS_READ_BYTE reads it
CLEAR LOAD_ET_FD_CPINIT SK_TMP
JEQ LOAD_ET_FD_CPCK SK_TMP RA_LOAD_RLEN LOAD_ET_FD_CPDONE
ITO LOAD_ET_FD_SRC   Add   El1=BS_TOKBUF_BASE      El2=SK_TMP         Exit=SK_TMP2
ITO LOAD_ET_FD_BYTE  Read  El1=SK_TMP2             Exit=SK_FLAG
ITO LOAD_ET_FD_DST   Add   El1=BS_READBUF_BASE     El2=SK_TMP         Exit=SK_TMP2
ITO LOAD_ET_FD_WR    Write El1=SK_TMP2             El2=SK_FLAG
ITO LOAD_ET_FD_INC2  Add   El1=SK_TMP              El2=C_1             Exit=SK_TMP
ITO LOAD_ET_FD_CPJMP Jump  Exit=LOAD_ET_FD_CPCK
NOLINK
RVOCA LOAD_ET_FD_CPDONE  LOAD_DISPATCH_LINE
ITO LOAD_ET_FD_RESET Move  El1=BS_TOKBUF_BASE     Exit=SK_ET_OUT
ITO LOAD_ET_FD_JMP   Jump  Exit=LOAD_ET_LOOP
NOLINK

── LOAD_EMIT_INT_BYTES: write decimal digits of SK_TMP → SK_ET_OUT ──
/Handles 0-9999.
NEWREF LOAD_EMIT_INT_BYTES LOAD_EIB_IMPL
NOLINK
ITO LOAD_EIB_IMPL    Move  El1=SK_TMP         Exit=SK_EIB_VAL
ITO LOAD_EIB_D100    Div   El1=SK_EIB_VAL    El2=C_100             Exit=SK_EIB_DIV
ITO LOAD_EIB_R100    Rem   El1=SK_EIB_VAL    El2=C_100             Exit=SK_EIB_VAL
JZ LOAD_EIB_CK100 SK_EIB_DIV LOAD_EIB_TENS
ITO LOAD_EIB_A100    Add   El1=SK_EIB_DIV    El2=ASCII_0            Exit=SK_TMP
WRITE_OUT LOAD_EIB_W100 SK_TMP
ITO LOAD_EIB_TENS    Div   El1=SK_EIB_VAL    El2=C_10              Exit=SK_EIB_DIV
ITO LOAD_EIB_RTENS   Rem   El1=SK_EIB_VAL    El2=C_10              Exit=SK_EIB_VAL
JZ LOAD_EIB_CKTENS SK_EIB_DIV LOAD_EIB_ONES
ITO LOAD_EIB_ATENS   Add   El1=SK_EIB_DIV    El2=ASCII_0            Exit=SK_TMP
WRITE_OUT LOAD_EIB_WTENS SK_TMP
ITO LOAD_EIB_ONES    Add   El1=SK_EIB_VAL    El2=ASCII_0            Exit=SK_TMP
WRITE_OUT LOAD_EIB_WONES SK_TMP
RREDI LOAD_EIB_RRET

── LOAD_PROCESS_BODY: process body lines for SAVE ────────────
NEWREF LOAD_PROCESS_BODY LOAD_PB_IMPL
NOLINK
ITO LOAD_PB_IMPL     Move  El1=SK_BODY_BASE    Exit=SK_PB_PTR
JEQ LOAD_PB_LOOP SK_PB_PTR SK_BODY_PTR LOAD_PB_DONE
ITO LOAD_PB_RBYTE    Read  El1=SK_PB_PTR        Exit=SK_TMP
JEQ LOAD_PB_MRK1     SK_TMP C_1 LOAD_PB_SKIP
JEQ LOAD_PB_MRK2     SK_TMP C_2 LOAD_PB_SKIP
JEQ LOAD_PB_LFCK SK_TMP LF LOAD_PB_DISPATCH
ITO LOAD_PB_INC      Add   El1=SK_PB_PTR         El2=C_1           Exit=SK_PB_PTR
ITO LOAD_PB_JMP      Jump  Exit=LOAD_PB_LOOP
NOLINK
ITO LOAD_PB_SKIP     Add   El1=SK_PB_PTR         El2=C_1           Exit=SK_PB_PTR
ITO LOAD_PB_SKIP_JMP Jump  Exit=LOAD_PB_LOOP
NOLINK
RVOCA LOAD_PB_DISPATCH  LOAD_DISPATCH_LINE
ITO LOAD_PB_INC2     Add   El1=SK_PB_PTR         El2=C_1           Exit=SK_PB_PTR
ITO LOAD_PB_JMP2     Jump  Exit=LOAD_PB_LOOP
NOLINK
RREDI LOAD_PB_DONE

── SAVE_EMIT_SAVES: alloc Move reg→S_reg ITO luces ───────────
NEWREF SAVE_EMIT_SAVES SAVE_ES_IMPL
NOLINK
JZ SAVE_ES_IMPL RA_MA1 SAVE_ES_DONE
ITO SAVE_ES_D1       Move  El1=SK_SES_SREG_1   Exit=SK_SES_SDST
RVOCA SAVE_ES_DO1    SAVE_EMIT_ONE_SAVE
JZ SAVE_ES_CK2 RA_MA2 SAVE_ES_DONE
ITO SAVE_ES_MV2      Move  El1=RA_MA2          Exit=RA_MA1
ITO SAVE_ES_D2       Move  El1=SK_SES_SREG_2   Exit=SK_SES_SDST
RVOCA SAVE_ES_DO2    SAVE_EMIT_ONE_SAVE
JZ SAVE_ES_CK3 RA_MA3 SAVE_ES_DONE
ITO SAVE_ES_MV3      Move  El1=RA_MA3          Exit=RA_MA1
ITO SAVE_ES_D3       Move  El1=SK_SES_SREG_3   Exit=SK_SES_SDST
RVOCA SAVE_ES_DO3    SAVE_EMIT_ONE_SAVE
JZ SAVE_ES_CK4 RA_MA4 SAVE_ES_DONE
ITO SAVE_ES_MV4      Move  El1=RA_MA4          Exit=RA_MA1
ITO SAVE_ES_D4       Move  El1=SK_SES_SREG_4   Exit=SK_SES_SDST
RVOCA SAVE_ES_DO4    SAVE_EMIT_ONE_SAVE
RREDI SAVE_ES_DONE

── SAVE_EMIT_ONE_SAVE: alloc S_reg lux + Move ITO ───────────
NEWREF SAVE_EMIT_ONE_SAVE SAVE_EOS_IMPL
NOLINK
ALLOC_TO SAVE_EOS_IMPL SK_SES_SREG C_1
ITO SAVE_EOS_SDST    Write El1=SK_SES_SDST    El2=SK_SES_SREG
ITO SAVE_EOS_ICN     Move  El1=ITO_SIZE       Exit=RA_ALLOC_COUNT
RVOCA SAVE_EOS_IAC   ALLOC_LUCES
ITO SAVE_EOS_ISELF   Write El1=RA_ALLOC_RESULT El2=RA_ALLOC_RESULT
FOR C_1 C_2 C_4
    ITO SAVE_EOS_I{N}    Add   El1=RA_ALLOC_RESULT El2={X}             Exit=SK_TMP
    ITO SAVE_EOS_I{N}W   Write El1=SK_TMP          El2={Y}
        C_1 > Y=Move
        C_2 > Y=RA_MA1
        > Y=SK_SES_SREG
ITO SAVE_EOS_LP      Move  El1=RA_MC_PREV      Exit=RA_NL_PREV
ITO SAVE_EOS_LN      Move  El1=RA_ALLOC_RESULT Exit=RA_NL_NEXT
RVOCA SAVE_EOS_LNK   LINK_NEXT
ITO SAVE_EOS_NOAUTO  Move  El1=RA_ALLOC_RESULT Exit=RA_MC_PREV
RREDI SAVE_EOS_RRET

── SAVE_EMIT_RESTORES: emit Move S_reg→reg luces ─────────────
NEWREF SAVE_EMIT_RESTORES SAVE_ER_IMPL
NOLINK
JZ SAVE_ER_IMPL RA_MA1 SAVE_ER_DONE
ITO SAVE_ER_SD1      Read  El1=SK_SES_SREG_1  Exit=SK_SES_SREG
RVOCA SAVE_ER_DO1    SAVE_EMIT_ONE_RESTORE
JZ SAVE_ER_CK2 RA_MA2 SAVE_ER_DONE
ITO SAVE_ER_MV2      Move  El1=RA_MA2         Exit=RA_MA1
ITO SAVE_ER_SD2      Read  El1=SK_SES_SREG_2  Exit=SK_SES_SREG
RVOCA SAVE_ER_DO2    SAVE_EMIT_ONE_RESTORE
JZ SAVE_ER_CK3 RA_MA3 SAVE_ER_DONE
ITO SAVE_ER_MV3      Move  El1=RA_MA3         Exit=RA_MA1
ITO SAVE_ER_SD3      Read  El1=SK_SES_SREG_3  Exit=SK_SES_SREG
RVOCA SAVE_ER_DO3    SAVE_EMIT_ONE_RESTORE
RREDI SAVE_ER_DONE

── SAVE_EMIT_ONE_RESTORE: alloc Move S_reg→reg ITO ───────────
NEWREF SAVE_EMIT_ONE_RESTORE SAVE_EOR_IMPL
NOLINK
ITO SAVE_EOR_IMPL    Move  El1=ITO_SIZE       Exit=RA_ALLOC_COUNT
RVOCA SAVE_EOR_IAC   ALLOC_LUCES
ITO SAVE_EOR_ISELF   Write El1=RA_ALLOC_RESULT El2=RA_ALLOC_RESULT
FOR C_1 C_2 C_4
    ITO SAVE_EOR_I{N}    Add   El1=RA_ALLOC_RESULT El2={X}             Exit=SK_TMP
    ITO SAVE_EOR_I{N}W   Write El1=SK_TMP          El2={Y}
        C_1 > Y=Move
        C_2 > Y=SK_SES_SREG
        > Y=RA_MA1
ITO SAVE_EOR_LP      Move  El1=RA_MC_PREV      Exit=RA_NL_PREV
ITO SAVE_EOR_LN      Move  El1=RA_ALLOC_RESULT Exit=RA_NL_NEXT
RVOCA SAVE_EOR_LNK   LINK_NEXT
ITO SAVE_EOR_NOAUTO  Move  El1=RA_ALLOC_RESULT Exit=RA_MC_PREV
RREDI SAVE_EOR_RRET
