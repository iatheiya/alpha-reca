//============================================================
/saku.re — Universal single-pass .re file loader

/Entry point: LOAD_MAIN
/  IN: BS_FILE_LIST = addr of file path array
/      BS_FILE_COUNT = number of files

/Single pass per file. Forward references via backfill list.
/Lazy alloc: unknown name → 1-lux placeholder → backfill later.

/Reuses from bootstrap.re:
/  BS_READ_BYTE, BS_READ_TOKEN, BS_LOOKUP, BS_INTERN
/  BS_TOKEN_VALUE, BS_SKIP_TO_EOL, BS_OPEN_FILE
/  BS_HT_BASE, BS_HT_MASK, BS_HT_SIZE (shared htable)
/  BS_LAST_ITO (autolink state)

/Implements stubs declared in macros.re:
/  LOAD_READ_LINE, LOAD_READ_TOKEN, LOAD_INTERN_TOKEN
/  LOAD_READ_BODY, LOAD_EXPAND_TEMPLATE, LOAD_PROCESS_BODY
/  SAVE_EMIT_SAVES, SAVE_EMIT_RESTORES

/DEPENDS ON: bootstrap.re constants.re registers.re alloc.re htable.re
//============================================================

/── Command handler registry ──────────────────────────────────
/These NEWREF declarations register command handlers in the htable.
/Sweep A finds them via BS_LOOKUP when dispatching command tokens.
NEWREF NEWREF  LOAD_CMD_NEWREF
NEWREF NEWSET  LOAD_CMD_NEWSET
NEWREF SETREF  LOAD_CMD_SETREF
NEWREF SET     LOAD_CMD_SET_IMPL
NEWREF RVOCA   LOAD_CMD_RVOCA_IMPL
NEWREF RREDI   LOAD_CMD_RREDI_IMPL
NEWREF NOLINK  LOAD_CMD_NOLINK
NEWREF ITO     LOAD_CMD_ITO
NEWREF BLOCK   LOAD_CMD_BLOCK
NEWREF LINK    LOAD_CMD_LINK_IMPL

/── Loader state ──────────────────────────────────────────────
NEW LD_FIDX          /current file index
NEW LD_FPATH         /current file path addr
NEW LD_BLOCK_CMT     /0=normal, 1=inside block comment
NEW LD_BACKFILL_BASE /start of backfill list in aether
NEW LD_BACKFILL_PTR  /current backfill write position
NEW LD_INDENT_DEPTH  /current indentation depth (0=none)
NEW LD_BODY_BASE     /base of body buffer (FOR/SAVE template)
NEW LD_BODY_PTR      /write position in body buffer
NEW LD_BODY_LINE_CNT /number of lines in body buffer
NEW LD_CMD_ADDR      /current command lux addr

/── Scratch ───────────────────────────────────────────────────
NEW LD_TMP
NEW LD_TMP2
NEW LD_TMP3
NEW LD_FLAG
SETREF LD_FLAG LD_FLAG  /self-ref flag for Equal→JumpIf chains

/── Backfill: (slot_addr, name_hash) pairs ───────────────────

/── Body buffer for FOR/SAVE template ───────────────────────
NEWSET LD_BODY_BUF_SIZE 4096
BLOCK  LD_BODY_BUF_000  4096
SETREF LD_BODY_BUF_000 LD_BODY_BUF_000  /self-ref
NEW    LD_BODY_BUF_BASE_VAL
NEW    LD_BODY_BUF_LIMIT  /base + size: bounds check sentinel

/── Backfill buffer ──────────────────────────────────────────
BLOCK  LD_BACKFILL_BUF_000  2048
SETREF LD_BACKFILL_BUF_000 LD_BACKFILL_BUF_000  /self-ref

/── Lumen count table: (name_addr, count) pairs ─────────────
/Prepass scans all LINK commands and records count per src name.
/Alloc uses this to size luces correctly (compact lumens in-place).
/Table is a flat array of pairs: [addr0, cnt0, addr1, cnt1, ..., 0]
/Max 1024 distinct src luces with LINK lumens.
BLOCK  LD_LCOUNT_BUF_000  2048    /1024 pairs * 2 luces each
SETREF LD_LCOUNT_BUF_000 LD_LCOUNT_BUF_000  /self-ref so Move reads addr
NEW    LD_LCOUNT_BASE      /base addr of lcount table
NEW    LD_LCOUNT_PTR       /write pointer (end of used pairs)

/── ASCII constants (local to avoid depending on ascii.re order) ─

/── Pack-string state (for SET Name "...") ────────────────────
/LD_PS_WORD   : accumulator — current u64 word being built
/LD_PS_SHIFT  : bit shift for next byte (0,8,16,...,56)
/LD_PS_FIRST  : addr of first allocated lux (return value)
/LD_PS_LAST   : addr of last allocated lux (for NUL terminator)
/LD_PS_NAMEDST: addr of name lux to write result into
NEW LD_PS_WORD
NEW LD_PS_SHIFT
NEW LD_PS_FIRST
NEW LD_PS_LAST
NEW LD_PS_NAMEDST
NEW LD_PS_ESCAPED  /flag: previous byte was backslash

/── LOAD_MAIN: entry point ────────────────────────────────────
NOLINK
/Init body buffer base
ITO LOAD_MAIN     Move  El1=LD_BODY_BUF_000  Exit=LD_BODY_BUF_BASE_VAL
ITO LOAD_MAIN_BBL    Add   El1=LD_BODY_BUF_BASE_VAL El2=LD_BODY_BUF_SIZE Exit=LD_BODY_BUF_LIMIT
/Init backfill
ITO LOAD_MAIN_BF     Move  El1=LD_BACKFILL_BUF_000 Exit=LD_BACKFILL_BASE
ITO LOAD_MAIN_BFP    Move  El1=LD_BACKFILL_BASE Exit=LD_BACKFILL_PTR
/Init lcount table
ITO LOAD_MAIN_LC     Move  El1=LD_LCOUNT_BUF_000 Exit=LD_LCOUNT_BASE
ITO LOAD_MAIN_LCP    Move  El1=LD_LCOUNT_BASE Exit=LD_LCOUNT_PTR
/Reset RA_MC_PREV (autolink state)
CLEAR LOAD_MAIN_MC RA_MC_PREV
/── Wave 1 (prepass): count LINK lumens per src name ────────
CLEAR LOAD_MAIN_PI LD_FIDX
ITO LOAD_MAIN_PLOOP  Equal El1=LD_FIDX      El2=BS_FILE_COUNT Exit=LD_FLAG
ITO LOAD_MAIN_PLCKJ  JumpIf El1=LD_FLAG     Exit=LOAD_MAIN_P2
ITO LOAD_MAIN_PFADDR Add   El1=BS_FILE_LIST El2=LD_FIDX       Exit=LD_TMP
ITO LOAD_MAIN_PFLOAD Read  El1=LD_TMP       Exit=LD_FPATH
ITO LOAD_MAIN_PFO    Move  El1=LOAD_MAIN_PLOOP Exit=RA_LINK
RVOCA LOAD_MAIN_PFF  LOAD_PREPASS_FILE
ITO LOAD_MAIN_PINC   Add   El1=LD_FIDX      El2=C_1           Exit=LD_FIDX
ITO LOAD_MAIN_PJMP   Jump  Exit=LOAD_MAIN_PLOOP
/── Wave 2 (load): build graph using lcount sizes ───────────
NOLINK
CLEAR LOAD_MAIN_P2 LD_FIDX
ITO LOAD_MAIN_LOOP   Equal El1=LD_FIDX      El2=BS_FILE_COUNT Exit=LD_FLAG
ITO LOAD_MAIN_LCKJ   JumpIf El1=LD_FLAG     Exit=LOAD_MAIN_BF2
ITO LOAD_MAIN_FADDR  Add   El1=BS_FILE_LIST El2=LD_FIDX       Exit=LD_TMP
ITO LOAD_MAIN_FLOAD  Read  El1=LD_TMP       Exit=LD_FPATH
RVOCA LOAD_MAIN_FILE LOAD_FILE
ITO LOAD_MAIN_INC    Add   El1=LD_FIDX      El2=C_1           Exit=LD_FIDX
ITO LOAD_MAIN_JMP    Jump  Exit=LOAD_MAIN_LOOP
/Apply backfill
NOLINK
RVOCA LOAD_MAIN_BF2   LOAD_APPLY_BACKFILL
RREDI LOAD_MAIN_RRET
/── LOAD_OPEN_FILE: open LD_FPATH O_RDONLY, init read buffer ──
/OUT: RA_LOAD_FD = file descriptor. Leaf.
NOLINK
ITO LOAD_OPEN_FILE  Move  El1=SYS_OPENAT   Exit=SC_NR
ITO LOF_DIR         Move  El1=AT_FDCWD     Exit=SC_A0
ITO LOF_PATH        Move  El1=LD_FPATH     Exit=SC_A1
ITO LOF_FLAGS       Move  El1=O_RDONLY     Exit=SC_A2
ITO LOF_XR          Exire El1=C_0          El2=C_0   Exit=C_0
ITO LOF_FD          Move  El1=SC_A0        Exit=RA_LOAD_FD
CLEAR LOF_RPOS      RA_LOAD_RPOS
CLEAR LOF_RLEN      RA_LOAD_RLEN
RREDI LOF_RET

/── LOAD_FILE: open, process, close one .re file ─────────────
NOLINK
/Open file and init read buffer
RVOCA LOAD_FILE      LOAD_OPEN_FILE
CLEAR LOAD_FO_BCMT LD_BLOCK_CMT
/Init indent depth
CLEAR LOAD_FO_IND LD_INDENT_DEPTH
/Process lines until EOF
RVOCA LOAD_FL_LOOP   LOAD_DISPATCH_LINE
/EOF check: BS_READ_TOKEN returns tlen=0 on EOF (or full-line comment, keep looping)
/Distinguish EOF from comment: BS_READ_TOKEN sets RA_LOAD_BYTE=0 on true EOF
JZ LOAD_FL_EOFCK RA_LOAD_BYTE LOAD_FL_DONE
ITO LOAD_FL_JMP      Jump  Exit=LOAD_FL_LOOP
/Close file
NOLINK
ITO LOAD_FL_DONE     Move  El1=SYS_CLOSE    Exit=SC_NR
ITO LOAD_FL_CLFD     Move  El1=RA_LOAD_FD   Exit=SC_A0
ITO LOAD_FL_CLXR     Exire El1=C_0          El2=C_0            Exit=C_0
RREDI LOAD_FL_RRET
/── LOAD_DISPATCH_LINE: read first token and dispatch ─────────
/BS_READ_TOKEN handles whitespace, // inline blocks, and / line comments.
/tlen==0 → EOF or full-line comment → return to caller.
/First byte of tokbuf drives dispatch.
NOLINK
/Read first token (BS_READ_TOKEN skips whitespace, // blocks, / comments)
RVOCA LOAD_DISPATCH_LINE BS_READ_TOKEN
/tlen==0 → EOF or full-line comment → return
JZ LOAD_DL_TLENCK RA_LOAD_TLEN LOAD_DL_RET
/Read first byte of token → LD_TMP (for SWITCH dispatch)
ITO LOAD_DL_FBTOK    Read  El1=BS_TOKBUF_BASE Exit=LD_TMP
/Lookup full token in htable → RA_BS_RESULT → LD_CMD_ADDR
RVOCA LOAD_DL_LK     BS_LOOKUP
ITO LOAD_DL_SAVEMD   Move  El1=RA_BS_RESULT Exit=LD_CMD_ADDR
/Dispatch by first byte of token
SWITCH LD_TMP
    ASCII_N  LOAD_CMD_N_GROUP
    ASCII_S  LOAD_CMD_SET_OR_SRF
    ASCII_L  LOAD_CMD_LINK_OR_LR
    ASCII_I  LOAD_CMD_ITO
    ASCII_B  LOAD_CMD_BLOCK
RVOCA LOAD_DL_UCMD   LOAD_CMD_UNKNOWN
RREDI LOAD_DL_RET
RVOCA LOAD_DL_INSKP  BS_SKIP_TO_EOL
RREDI LOAD_DL_IRRET
/Skip comment line
NOLINK
RVOCA LOAD_DL_SSKP   BS_SKIP_TO_EOL
RREDI LOAD_DL_SRRET
/── LOAD_DL_SL2: second byte after '/' ──────────────────────────
/If second byte is '/' → block comment (possibly multi-line)
/Otherwise → single-line comment (skip to EOL)
NOLINK
RVOCA LOAD_DL_SL2RB  BS_READ_BYTE
ITO LOAD_DL_SL2CK    Equal El1=RA_LOAD_BYTE El2=SLASH      Exit=LD_FLAG
ITO LOAD_DL_SL2J     JumpIf El1=LD_FLAG     Exit=LOAD_DL_BLOCK
/Single '/' → skip to EOL
RVOCA LOAD_DL_SL2SK  BS_SKIP_TO_EOL
RREDI LOAD_DL_SL2SRR
/── LOAD_DL_BLOCK: inside block comment
/Read bytes until closing double-slash. Handles multi-line blocks.
NOLINK
RVOCA LOAD_DL_BLOCK  BS_READ_BYTE
/EOF inside block → treat as end
JZ LOAD_DL_BEOFCK RA_LOAD_BYTE LOAD_DL_BEOF
/Is it '/'?
ITO LOAD_DL_BSLCK    Equal El1=RA_LOAD_BYTE El2=SLASH      Exit=LD_FLAG
ITO LOAD_DL_BSLJ     JumpIf El1=LD_FLAG     Exit=LOAD_DL_BSL2
/Not '/' → keep scanning
ITO LOAD_DL_BJMP     Jump  Exit=LOAD_DL_BLOCK
/Got first '/': check next byte
NOLINK
RVOCA LOAD_DL_BSL2   BS_READ_BYTE
/EOF after '/' → done
JZ LOAD_DL_BSL2EOF RA_LOAD_BYTE LOAD_DL_BEOF
/Second '/' → closing double-slash found, skip rest of line, return
ITO LOAD_DL_BSL2CK   Equal El1=RA_LOAD_BYTE El2=SLASH      Exit=LD_FLAG
ITO LOAD_DL_BSL2J    JumpIf El1=LD_FLAG     Exit=LOAD_DL_BEND
/Not second '/' → check if this byte is itself '/' (could start new pair)
ITO LOAD_DL_BSL2NKJ  JumpIf El1=LD_FLAG     Exit=LOAD_DL_BSL2
ITO LOAD_DL_BSL2NJP  Jump  Exit=LOAD_DL_BLOCK
/Block closed: skip rest of line, return
NOLINK
RVOCA LOAD_DL_BEND   BS_SKIP_TO_EOL
RREDI LOAD_DL_BRRET
/EOF inside block → return
NOLINK
RREDI LOAD_DL_BEOF
/── LOAD_DL_COLLECT: read remaining bytes of token (after first byte written)
/Appends bytes to tokbuf[tlen..] updating hash, stops at space/LF/EOF/slash.
/Does NOT reset tlen or hash. Leaves RA_LOAD_BYTE = terminator.
/Called after peeked byte is in tokbuf[0] and tlen=1.
NOLINK
NEWREF LOAD_DL_COLLECT LOAD_DL_CO_RD  /alias/
RVOCA LOAD_DL_CO_RD  BS_READ_BYTE
/EOF?
JZ LOAD_DL_CO_EOFCK RA_LOAD_BYTE LOAD_DL_CO_DONE
/LF?
ITO LOAD_DL_CO_LFCK  Equal El1=RA_LOAD_BYTE El2=LF         Exit=LD_FLAG
ITO LOAD_DL_CO_LFJ   JumpIf El1=LD_FLAG     Exit=LOAD_DL_CO_DONE
/Space?
ITO LOAD_DL_CO_SPCK  Equal El1=RA_LOAD_BYTE El2=SP      Exit=LD_FLAG
ITO LOAD_DL_CO_SPCKJ JumpIf El1=LD_FLAG     Exit=LOAD_DL_CO_DONE
/Tab?
ITO LOAD_DL_CO_TBCK  Equal El1=RA_LOAD_BYTE El2=TAB        Exit=LD_FLAG
ITO LOAD_DL_CO_TBCKJ JumpIf El1=LD_FLAG     Exit=LOAD_DL_CO_DONE
/Slash? — end token and skip rest of line (handled by BS_RT_SLCMT logic)
ITO LOAD_DL_CO_SLCK  Equal El1=RA_LOAD_BYTE El2=SLASH      Exit=LD_FLAG
ITO LOAD_DL_CO_SLCKJ JumpIf El1=LD_FLAG     Exit=LOAD_DL_CO_SL
/Normal byte: write to tokbuf[tlen]
ITO LOAD_DL_CO_TBOF  Add   El1=BS_TOKBUF_BASE El2=RA_LOAD_TLEN Exit=RA_BS_TMP
ITO LOAD_DL_CO_TBWR  Write El1=RA_BS_TMP   El2=RA_LOAD_BYTE
/Update hash: hash = (hash*33 + byte) & mask
ITO LOAD_DL_CO_H33   Mul   El1=RA_LOAD_HASH El2=C_33          Exit=RA_BS_TMP2
ITO LOAD_DL_CO_HADD  Add   El1=RA_BS_TMP2  El2=RA_LOAD_BYTE   Exit=RA_LOAD_HASH
ITO LOAD_DL_CO_HMSK  And   El1=RA_LOAD_HASH El2=BS_HT_MASK    Exit=RA_LOAD_HASH
/tlen++
ITO LOAD_DL_CO_INC   Add   El1=RA_LOAD_TLEN El2=C_1           Exit=RA_LOAD_TLEN
ITO LOAD_DL_CO_LOOP  Jump  Exit=LOAD_DL_CO_RD
/Slash mid-token: skip rest of line
NOLINK
RVOCA LOAD_DL_CO_SL  BS_SKIP_TO_EOL
RREDI LOAD_DL_CO_SLRR
NOLINK
RREDI LOAD_DL_CO_DONE
/── LOAD_CMD_N_GROUP: dispatch N* commands ────────────────────
/NEW(3), NEWREF(6), NEWSET(6), NEXO(4), NOLINK(6)
/Second byte: O→NOLINK, E→NEW(len=3) or NEWREF/NEWSET/NEXO via macro
CHAIN
    LOAD_CMD_N_GROUP   Add   El1=BS_TOKBUF_BASE El2=C_1         Exit=LD_TMP
    LOAD_CNG_B1        Read  El1=LD_TMP        Exit=LD_TMP
        SWITCH LD_TMP
            ASCII_O LOAD_CMD_NOLINK
            ASCII_E LOAD_CNG_NE
        RVOCA LOAD_CNG_UCMD LOAD_CMD_UNKNOWN
        RREDI LOAD_CNG_URRET
/NE*: len==3 → NEW, else → NEWREF/NEWSET/NEXO via macro dispatch
NOLINK
ITO LOAD_CNG_NE     Equal El1=RA_LOAD_TLEN  El2=C_3          Exit=LD_FLAG
ITO LOAD_CNG_NEJ    JumpIf El1=LD_FLAG      Exit=LOAD_CMD_NEW
/len>3: NEWREF/NEWSET/NEXO — Reca programs handle their els
RVOCA LOAD_CNG_LU   LOAD_CMD_UNKNOWN
RREDI LOAD_CNG_LRRET
/── LOAD_CMD_NEW: NEW name ────────────────────────────────────
/Reads name token. Alloc 1 + 2*lcount + (lcount>0?1:0) luces.
/If already in htable: confirm only (no realloc).
NOLINK
RVOCA LOAD_CMD_NEW   BS_READ_TOKEN
JZ LOAD_CN_CKL RA_LOAD_TLEN LOAD_CN_DONE
/BS_READ_TOKEN set RA_LOAD_HASH — use it for lcount lookup
/Get lcount
RVOCA LOAD_CN_LCGET  LOAD_LCOUNT_GET
/LD_TMP = lcount. Compute size: 1 + 2*lcount + (lcount>0?1:0)
ITO LOAD_CN_SZ1      Mul   El1=LD_TMP       El2=C_2           Exit=LD_FLAG
ITO LOAD_CN_SZ2      Add   El1=C_1          El2=LD_FLAG        Exit=RA_ALLOC_COUNT
JZ LOAD_CN_TCKZ LD_TMP LOAD_CN_INTERN
ITO LOAD_CN_TADD     Add   El1=RA_ALLOC_COUNT El2=C_1         Exit=RA_ALLOC_COUNT
/Intern: BS_INTERN does lazy alloc only if absent. But we need sized alloc.
/So: lookup first, if absent alloc manually and register.
RVOCA LOAD_CN_INTERN   BS_LOOKUP
JZ LOAD_CN_LKCK RA_BS_RESULT LOAD_CN_ALLOC
/Already exists: return (caller handles EOL skip)
RREDI LOAD_CN_DONE
/Not found: alloc sized block and register
NOLINK
RVOCA LOAD_CN_ALLOC   ALLOC_LUCES
/Save user-lux addr before BS_PACK_TOKBUF clobbers RA_ALLOC_RESULT
ITO LOAD_CN_SAV      Move  El1=RA_ALLOC_RESULT Exit=RA_BS_EL0
/Pack token → packed string addr in RA_BS_TMP2
RVOCA LOAD_CN_PACK   BS_PACK_TOKBUF
/Restore user-lux addr, write packed name to lux[1]
ITO LOAD_CN_RST      Move  El1=RA_BS_EL0   Exit=RA_BS_RESULT
ITO LOAD_CN_N1A      Add   El1=RA_BS_RESULT El2=C_1           Exit=RA_BS_TMP
ITO LOAD_CN_N1W      Write El1=RA_BS_TMP    El2=RA_BS_TMP2
/Insert user-lux into htable
ITO LOAD_CN_HH       Move  El1=RA_LOAD_HASH Exit=RA_HT_HASH
ITO LOAD_CN_HL       Move  El1=RA_BS_RESULT Exit=RA_HT_LID
ITO LOAD_CN_HB       Move  El1=BS_HT_BASE   Exit=RA_HT_BASE
ITO LOAD_CN_HM       Move  El1=BS_HT_MASK   Exit=RA_HT_MASK
ITO LOAD_CN_HS       Move  El1=BS_HT_SIZE   Exit=RA_HT_SIZE
RVOCA LOAD_CN_HT     HT_INSERT
RVOCA LOAD_CN_ASKP   BS_SKIP_TO_EOL
RREDI LOAD_CN_ARRET
/── LOAD_CMD_SET_OR_SRF: SET / SETREF / SAVE ─────────────────
/Check token length and second byte to distinguish.
NOLINK
/Check second byte: E→SET/SETREF, A→SAVE
ITO LOAD_CMD_SET_OR_SRF   Add   El1=BS_TOKBUF_BASE El2=C_1         Exit=LD_TMP
ITO LOAD_CSS_B1      Read  El1=LD_TMP       Exit=LD_TMP
ITO LOAD_CSS_CKE     Equal El1=LD_TMP       El2=ASCII_E   Exit=LD_FLAG
ITO LOAD_CSS_CKEJ    JumpIf El1=LD_FLAG     Exit=LOAD_CSS_SETCK
ITO LOAD_CSS_CKA     Equal El1=LD_TMP       El2=ASCII_A     Exit=LD_FLAG
ITO LOAD_CSS_CKAJ    JumpIf El1=LD_FLAG     Exit=LOAD_CMD_SAVE_CALL
/Unknown S* command → macro dispatch
RVOCA LOAD_CSS_UCMD  LOAD_CMD_UNKNOWN
RREDI LOAD_CSS_RRET
/Second byte was 'E': distinguish SET (tlen=3) from SETREF (tlen>3)
NOLINK
ITO LOAD_CSS_SETCK   Equal El1=RA_LOAD_TLEN El2=C_3 Exit=LD_FLAG
ITO LOAD_CSS_SETCKJ  JumpIf El1=LD_FLAG     Exit=LOAD_CMD_SET_IMPL
/tlen!=3 → SETREF or other S+E command → macro dispatch
RVOCA LOAD_CSS_SEUCMD  LOAD_CMD_UNKNOWN
RREDI LOAD_CSS_SERRET
/SAVE → call SAVE program
NOLINK
RVOCA LOAD_CMD_SAVE_CALL   SAVE
RREDI LOAD_SAVE_RRET
/── LOAD_CMD_SET_IMPL: SET name value ────────────────────────
/name → intern → get addr. value → intern or parse int or pack string → write.
NOLINK
/Read name
RVOCA LOAD_CMD_SET_IMPL   BS_READ_TOKEN
RVOCA LOAD_CS_NINT   BS_INTERN
ITO LOAD_CS_NADDR    Move  El1=RA_BS_RESULT Exit=LD_TMP
/Read value token (or first byte for quoted string detection)
/Peek first byte to check for '"'
RVOCA LOAD_CS_VTOK   BS_READ_TOKEN
/Check if quoted string: first byte = '"' (34)
ITO LOAD_CS_VB0      Read  El1=BS_TOKBUF_BASE Exit=LD_TMP2
ITO LOAD_CS_QCK      Equal El1=LD_TMP2      El2=DQUOTE     Exit=LD_FLAG
ITO LOAD_CS_QCKJ     JumpIf El1=LD_FLAG     Exit=LOAD_CS_STR
/Check if integer: first byte digit or '-'
ITO LOAD_CS_DASH     Equal El1=LD_TMP2      El2=MINUS       Exit=LD_FLAG
ITO LOAD_CS_DASHJ    JumpIf El1=LD_FLAG     Exit=LOAD_CS_INT
ITO LOAD_CS_D0       ULess El1=LD_TMP2      El2=ASCII_0       Exit=LD_FLAG
ITO LOAD_CS_D0J      JumpIf El1=LD_FLAG     Exit=LOAD_CS_SYM
ITO LOAD_CS_D9       ULess El1=ASCII_9      El2=LD_TMP2       Exit=LD_FLAG
ITO LOAD_CS_D9J      JumpIf El1=LD_FLAG     Exit=LOAD_CS_SYM
/Integer: parse and write directly
NOLINK
RVOCA LOAD_CS_INT   BS_PARSE_INT
ITO LOAD_CS_WINT     Write El1=LD_TMP       El2=RA_BS_TMP2
RVOCA LOAD_CS_ISKLS  BS_SKIP_TO_EOL
RREDI LOAD_CS_IRRET
/Symbol: intern and write addr
NOLINK
RVOCA LOAD_CS_SYM   BS_INTERN
ITO LOAD_CS_WSYM     Write El1=LD_TMP       El2=RA_BS_RESULT
RVOCA LOAD_CS_SSKLS  BS_SKIP_TO_EOL
RREDI LOAD_CS_SRRET
/Quoted string: read until closing '"', pack bytes, write first lux addr
NOLINK
ITO LOAD_CS_STR      Move  El1=LD_TMP       Exit=LD_PS_NAMEDST
RVOCA LOAD_CS_STRP   LOAD_PACK_STRING
RVOCA LOAD_CS_SSKLS2 BS_SKIP_TO_EOL
RREDI LOAD_CS_SRRET2
/── LOAD_PACK_STRING: read quoted string bytes, pack 8-per-lux ─
/Called after BS_READ_TOKEN already read the opening '"' token.
/Reads raw bytes from file until unescaped '"'. Handles \n \t \\.
/Packs bytes little-endian 8 per lux via ALLOC_LUCES(1).
/Writes addr of first lux into LD_PS_NAMEDST.
/
/State: LD_PS_WORD (accumulator), LD_PS_SHIFT (0..56),
/       LD_PS_FIRST (first lux addr), LD_PS_LAST (last lux addr)
NOLINK
/Init pack state
CLEAR LOAD_PACK_STRING LD_PS_WORD
ITO LOAD_LPS_INIT1   Move  El1=C_1          Exit=LD_PS_SHIFT
CLEAR LOAD_LPS_INIT2 LD_PS_FIRST
CLEAR LOAD_LPS_INIT3 LD_PS_LAST
CLEAR LOAD_LPS_INIT4 LD_PS_ESCAPED
/Read loop: one byte at a time from file
RVOCA LOAD_LPS_LOOP  BS_READ_BYTE
/EOF or LF without closing quote: stop (treat as end of string)
JZ LOAD_LPS_EOFCK RA_LOAD_BYTE LOAD_LPS_FLUSH
ITO LOAD_LPS_LFCK    Equal El1=RA_LOAD_BYTE El2=LF         Exit=LD_FLAG
ITO LOAD_LPS_LFJ     JumpIf El1=LD_FLAG     Exit=LOAD_LPS_FLUSH
/Escaped byte?
ITO LOAD_LPS_ESCCK   Equal El1=LD_PS_ESCAPED El2=C_1          Exit=LD_FLAG
ITO LOAD_LPS_ESCJ    JumpIf El1=LD_FLAG     Exit=LOAD_LPS_ESC
/Not escaped: check for backslash
ITO LOAD_LPS_BSCK    Equal El1=RA_LOAD_BYTE El2=BACKSLASH  Exit=LD_FLAG
ITO LOAD_LPS_BSCKJ   JumpIf El1=LD_FLAG     Exit=LOAD_LPS_SETESC
/Check for closing '"'
ITO LOAD_LPS_DQCK    Equal El1=RA_LOAD_BYTE El2=DQUOTE     Exit=LD_FLAG
ITO LOAD_LPS_DQCKJ   JumpIf El1=LD_FLAG     Exit=LOAD_LPS_FLUSH
/Ordinary byte: emit it
RVOCA LOAD_LPS_EB    LOAD_PS_EMIT_BYTE
ITO LOAD_LPS_JMP     Jump  Exit=LOAD_LPS_LOOP
/Set escape flag
NOLINK
ITO LOAD_LPS_SETESC  Move  El1=C_1          Exit=LD_PS_ESCAPED
ITO LOAD_LPS_SEJMP   Jump  Exit=LOAD_LPS_LOOP
/Escaped byte: decode escape sequence
NOLINK
CLEAR LOAD_LPS_ESC LD_PS_ESCAPED
/Check 'n' → LF(10)
ITO LOAD_LPS_EN      Equal El1=RA_LOAD_BYTE El2=ASCII_nl     Exit=LD_FLAG
ITO LOAD_LPS_ENJ     JumpIf El1=LD_FLAG     Exit=LOAD_LPS_LF
/Check 't' → TAB(9)
ITO LOAD_LPS_ET      Equal El1=RA_LOAD_BYTE El2=ASCII_tl     Exit=LD_FLAG
ITO LOAD_LPS_ETJ     JumpIf El1=LD_FLAG     Exit=LOAD_LPS_TAB
/Default: emit the byte as-is (handles '\\' → '\', etc.)
RVOCA LOAD_LPS_EEB   LOAD_PS_EMIT_BYTE
ITO LOAD_LPS_EDJMP   Jump  Exit=LOAD_LPS_LOOP
/Emit LF
NOLINK
ITO LOAD_LPS_LF      Move  El1=LF         Exit=RA_LOAD_BYTE
RVOCA LOAD_LPS_LFEB  LOAD_PS_EMIT_BYTE
ITO LOAD_LPS_LFJMP   Jump  Exit=LOAD_LPS_LOOP
/Emit TAB
NOLINK
ITO LOAD_LPS_TAB     Move  El1=TAB         Exit=RA_LOAD_BYTE
RVOCA LOAD_LPS_TABEB LOAD_PS_EMIT_BYTE
ITO LOAD_LPS_TABJMP  Jump  Exit=LOAD_LPS_LOOP
/Flush: emit current partial word + NUL terminator lux
NOLINK
/If shift>0: flush partial word
JZ LOAD_LPS_FLUSH LD_PS_SHIFT LOAD_LPS_NUL
/Alloc 1 lux, write partial word
ITO LOAD_LPS_FA1     Move  El1=C_1            Exit=RA_ALLOC_COUNT
RVOCA LOAD_LPS_FAC   ALLOC_LUCES
/Write word into new lux
ITO LOAD_LPS_FWW     Write El1=RA_ALLOC_RESULT El2=LD_PS_WORD
/Update first/last
JZ LOAD_LPS_FCHKF LD_PS_FIRST LOAD_LPS_FSETF
ITO LOAD_LPS_FSKIP   Jump  Exit=LOAD_LPS_NUL
NOLINK
ITO LOAD_LPS_FSETF   Move  El1=RA_ALLOC_RESULT Exit=LD_PS_FIRST
/Alloc NUL terminator lux (word=0, already zeroed by bump)
ITO LOAD_LPS_NUL     Move  El1=C_1            Exit=RA_ALLOC_COUNT
RVOCA LOAD_LPS_NACC  ALLOC_LUCES
/If first lux never set: string was empty, use NUL lux as first
JZ LOAD_LPS_NCHKF LD_PS_FIRST LOAD_LPS_NSETE
ITO LOAD_LPS_NJMP    Jump  Exit=LOAD_LPS_WRITE
NOLINK
ITO LOAD_LPS_NSETE   Move  El1=RA_ALLOC_RESULT Exit=LD_PS_FIRST
/Write LD_PS_FIRST into LD_PS_NAMEDST
ITO LOAD_LPS_WRITE   Write El1=LD_PS_NAMEDST  El2=LD_PS_FIRST
RREDI LOAD_LPS_RRET
/── LOAD_PS_EMIT_BYTE: pack one byte into LD_PS_WORD ─────────
/IN: RA_LOAD_BYTE = byte to pack
/Packs byte at LD_PS_SHIFT position in LD_PS_WORD (little-endian).
/When shift reaches 64: flush word to a new lux, reset state.
NOLINK
/Compute byte << shift: use Mul (byte * 2^shift = byte * (1 << shift))
/2^shift: precomputed via shift counter. Shift is 0,8,16,...,56.
/Multiply byte by (1 << LD_PS_SHIFT): need power of 2.
/Use repeated doubling approach via Mul byte * shift_mult where shift_mult = 2^shift.
/Simpler: maintain LD_PS_SHIFT as actual bit count, compute 1<<shift via a lookup.
/For 8-bit alignment (shift = 0,8,16,24,32,40,48,56):
/  shift_mult = 1, 256, 65536, 16777216, ... = 256^(shift/8)
/We store shift as 0..7 (byte index 0..7), multiply = 256^index.
/LD_PS_SHIFT here is actually the byte index (0-7), not bit count.
/byte_val << (index*8) = byte_val * (256^index)
/We compute 256^index iteratively via LD_PS_SHIFT storing the actual multiplier.
/Redefine: LD_PS_SHIFT = current multiplier (1 initially, *256 each byte).
/
/Pack: LD_PS_WORD += RA_LOAD_BYTE * LD_PS_SHIFT
ITO LOAD_PS_EMIT_BYTE    Mul   El1=RA_LOAD_BYTE  El2=LD_PS_SHIFT   Exit=LD_TMP
ITO LOAD_LPEB_ADD    Add   El1=LD_PS_WORD    El2=LD_TMP         Exit=LD_PS_WORD
/Advance shift multiplier: *256
ITO LOAD_LPEB_SMUL   Mul   El1=LD_PS_SHIFT   El2=C_256   Exit=LD_PS_SHIFT
/Check if we've filled 8 bytes: shift multiplier = 256^8 = 2^64 wraps to 0
JZ LOAD_LPEB_SZCK LD_PS_SHIFT LOAD_LPEB_FLUSH
RREDI LOAD_LPEB_RRET
/Flush: allocate 1 lux, write current word, reset
NOLINK
ITO LOAD_LPEB_FLUSH  Move  El1=C_1            Exit=RA_ALLOC_COUNT
RVOCA LOAD_LPEB_AC   ALLOC_LUCES
ITO LOAD_LPEB_WW     Write El1=RA_ALLOC_RESULT El2=LD_PS_WORD
/Track first lux
JZ LOAD_LPEB_FCHK LD_PS_FIRST LOAD_LPEB_SETF
ITO LOAD_LPEB_FSKIP  Jump  Exit=LOAD_LPEB_RESET
NOLINK
ITO LOAD_LPEB_SETF   Move  El1=RA_ALLOC_RESULT Exit=LD_PS_FIRST
CLEAR LOAD_LPEB_RESET LD_PS_WORD
/Reset shift to 1 (multiplier for byte index 0)
ITO LOAD_LPEB_SRST   Move  El1=C_1            Exit=LD_PS_SHIFT
RREDI LOAD_LPEB_RRET3
/── LOAD_CMD_LINK_OR_LR: LINK / LR / LT / LX / LH ──────────
/Check second byte: I→LINK, R→LR, T→LT, X→LX, H→LH
CHAIN
    LOAD_CMD_LINK_OR_LR   Add   El1=BS_TOKBUF_BASE El2=C_1  Exit=LD_TMP
    LOAD_CLR_B1           Read  El1=LD_TMP                  Exit=LD_TMP
        JEQ LOAD_CLR_CKI LD_TMP ASCII_I LOAD_CMD_LINK_IMPL
        RVOCA LOAD_CLR_UCMD  LOAD_CMD_UNKNOWN
        RREDI LOAD_CLR_RRET

/── LOAD_CMD_LINK_IMPL: LINK src rel tgt ─────────────────────
/Alloc 2 luces for lumen pair, write rel/tgt, add to src.
NOLINK
/Read src
RVOCA LOAD_CMD_LINK_IMPL   BS_READ_TOKEN
RVOCA LOAD_CL_SI     BS_INTERN
ITO LOAD_CL_SRC      Move  El1=RA_BS_RESULT Exit=LD_TMP
/Read rel
RVOCA LOAD_CL_R      BS_READ_TOKEN
RVOCA LOAD_CL_RI     BS_INTERN
ITO LOAD_CL_REL      Move  El1=RA_BS_RESULT Exit=LD_TMP2
/Read tgt
RVOCA LOAD_CL_T      BS_READ_TOKEN
RVOCA LOAD_CL_TI     BS_INTERN
ITO LOAD_CL_TGT      Move  El1=RA_BS_RESULT Exit=LD_TMP3
/Alloc 2 luces for (rel, exit) lumen pair
ITO LOAD_CL_CNT      Move  El1=C_2          Exit=RA_ALLOC_COUNT
RVOCA LOAD_CL_AC     ALLOC_LUCES
ITO LOAD_CL_WR       Write El1=RA_ALLOC_RESULT El2=LD_TMP2
ITO LOAD_CL_TA       Add   El1=RA_ALLOC_RESULT El2=C_1        Exit=LD_FLAG
ITO LOAD_CL_WT       Write El1=LD_FLAG       El2=LD_TMP3
/Skip rest of line
RVOCA LOAD_CL_SKLS   BS_SKIP_TO_EOL
RREDI LOAD_CL_RRET
/── LOAD_CMD_ITO: ITO name op [El1=x] [El2=y] [Exit=z] ──────
/Alloc ITO_SIZE + 2*lcount + (lcount>0?1:0) luces.
/Wire slots by key=value els. Autolink from RA_MC_PREV via slot 5.
NEW LD_ITO_ADDR
NEW LD_CI_NEW          /1=new alloc, 0=existing (autolink guard)
NEW LD_ITO_E1
NEW LD_ITO_E2
NEW LD_ITO_EXIT
NEW LD_ITO_LCNT  /lumen count from prepass
NOLINK
/Read name — BS_READ_TOKEN sets RA_LOAD_HASH = djb2 hash of name
RVOCA LOAD_CMD_ITO   BS_READ_TOKEN
/Get lcount for this name using its hash
RVOCA LOAD_CI_LCGET  LOAD_LCOUNT_GET
/LD_ITO_LCNT = lcount for this name (precomputed in prepass)
ITO LOAD_CI_LCSAVE   Move  El1=LD_TMP       Exit=LD_ITO_LCNT
/Intern name token → RA_BS_RESULT = name_lux addr (lazy-alloc if first time)
RVOCA LOAD_CI_NINT   BS_INTERN
ITO LOAD_CI_NADDR    Move  El1=RA_BS_RESULT Exit=LD_TMP
/Read word(name_lux): 0 = new ITO needed, non-zero = existing ITO addr
ITO LOAD_CI_EWGET    Read  El1=LD_TMP       Exit=LD_ITO_ADDR
/If existing (word != 0): read op token, then go to SKIP_ALLOC (reuse existing ITO)
JZ LOAD_CI_EWCK LD_ITO_ADDR LOAD_CI_OPTOK_NEW
/Existing path: setup RA_LINK then read op
RVOCA LOAD_CI_OPTOK  BS_READ_TOKEN
RVOCA LOAD_CI_OPINT  BS_INTERN
CLEAR LOAD_CI_EXCLR LD_CI_NEW
ITO LOAD_CI_EXJMP    Jump  Exit=LOAD_CI_SKIP_ALLOC
/New ITO: read op token then compute alloc size
NOLINK
RVOCA LOAD_CI_OPTOK_NEW  BS_READ_TOKEN
RVOCA LOAD_CI_OPINTN BS_INTERN
/If op == __LT_ALLOC_ITO: wire as Voca SR_GLR RA_LINK (special loader ITO type)
ITO LOAD_CI_ALTCK    Equal El1=RA_BS_RESULT El2=LD_ALLOC_ITO_OP Exit=LD_FLAG
ITO LOAD_CI_ALTCJ    JumpIf El1=LD_FLAG     Exit=LOAD_CI_ALTOP
/Compute alloc size: ITO_SIZE + 2*lcount + (lcount>0 ? 1 : 0)
ITO LOAD_CI_SZ1      Mul   El1=LD_ITO_LCNT  El2=C_2           Exit=LD_FLAG
ITO LOAD_CI_SZ2      Add   El1=ITO_SIZE     El2=LD_FLAG        Exit=RA_ALLOC_COUNT
JZ LOAD_CI_TCKZ LD_ITO_LCNT LOAD_CI_ALLOC
ITO LOAD_CI_TADD     Add   El1=RA_ALLOC_COUNT El2=C_1         Exit=RA_ALLOC_COUNT
ITO LOAD_CI_TJMP     Jump  Exit=LOAD_CI_ALLOC
NOLINK
/LOAD_CI_ALTOP: __LT_ALLOC_ITO → wire as Voca SR_GLR RA_LINK
ITO LOAD_CI_ALTOP    Move  El1=Voca       Exit=RA_BS_RESULT
ITO LOAD_CI_ALTE1    Move  El1=SR_GLR     Exit=LD_ITO_E1
ITO LOAD_CI_ALTEX    Move  El1=RA_LINK    Exit=LD_ITO_EXIT
ITO LOAD_CI_ALTJMP   Jump  Exit=LOAD_CI_ALLOC
NOLINK
/Allocate new ITO luces
RVOCA LOAD_CI_ALLOC  ALLOC_LUCES
ITO LOAD_CI_SAVEADDR Move  El1=RA_ALLOC_RESULT Exit=LD_ITO_ADDR
/Write ito_addr into name_lux.word (links name → ITO)
ITO LOAD_CI_REGN     Write El1=LD_TMP       El2=LD_ITO_ADDR
/Self-ref: aether[ito_addr] = ito_addr
ITO LOAD_CI_SELF     Write El1=LD_ITO_ADDR  El2=LD_ITO_ADDR
/New-alloc path falls through to LOAD_CI_SKIP_ALLOC (existing path entry)
ITO LOAD_CI_NEWSET   Move  El1=C_1          Exit=LD_CI_NEW
ITO LOAD_CI_NEWJMP   Jump  Exit=LOAD_CI_SKIP_ALLOC
/LOAD_CI_SKIP_ALLOC: entry for existing ITO (skips alloc/regn/self); write op
NOLINK
ITO LOAD_CI_SKIP_ALLOC Add El1=LD_ITO_ADDR El2=C_1           Exit=LD_FLAG
ITO LOAD_CI_OPW      Write El1=LD_FLAG       El2=RA_BS_RESULT
/Clear e1/e2/exit defaults
CLEAR LOAD_CI_CE1 LD_ITO_E1
CLEAR LOAD_CI_CE2 LD_ITO_E2
CLEAR LOAD_CI_CEX LD_ITO_EXIT
/Read key=value els
RVOCA LOAD_CI_ARGS   LOAD_ITO_READ_ARGS
/Write e1 at slot 2
ITO LOAD_CI_E1S      Add   El1=LD_ITO_ADDR  El2=C_2           Exit=LD_FLAG
ITO LOAD_CI_E1W      Write El1=LD_FLAG       El2=LD_ITO_E1
/Write e2 at slot 3
ITO LOAD_CI_E2S      Add   El1=LD_ITO_ADDR  El2=C_3           Exit=LD_FLAG
ITO LOAD_CI_E2W      Write El1=LD_FLAG       El2=LD_ITO_E2
/Write exit at slot 4
ITO LOAD_CI_EXS      Add   El1=LD_ITO_ADDR  El2=C_4           Exit=LD_FLAG
ITO LOAD_CI_EXW      Write El1=LD_FLAG       El2=LD_ITO_EXIT
/Autolink: skip entirely for existing luces (LD_CI_NEW=0), only link new allocs
JZ LOAD_CI_NEWCK LD_CI_NEW LOAD_CI_RET
/Autolink: if RA_MC_PREV != 0, write ito_addr into prev.next (slot 5)
JZ LOAD_CI_LCKZ RA_MC_PREV LOAD_CI_NOAUTO
ITO LOAD_CI_LADD     Add   El1=RA_MC_PREV   El2=C_5           Exit=LD_FLAG
ITO LOAD_CI_LW       Write El1=LD_FLAG       El2=LD_ITO_ADDR
ITO LOAD_CI_NOAUTO   Move  El1=LD_ITO_ADDR  Exit=RA_MC_PREV
RREDI LOAD_CI_RET
/── LOAD_ITO_READ_ARGS: read El1=/El2=/Exit= key=value tokens ─
/Sets LD_ITO_E1, LD_ITO_E2, LD_ITO_EXIT based on key.
/Key detection: read B0=E, B1=l→El*, B1=x→Exit, before stripping '='.
/El digit: B2='1'→slot2(E1), B2='2'→slot3(E2).
NEW LD_IRA_KEY1   /second byte of key (l or x)
NEW LD_IRA_KEY2   /third byte of key (digit: 1 or 2)
NOLINK
NEWREF LOAD_ITO_READ_ARGS LOAD_IRA_LOOP  /alias/
ITO LOAD_IRA_LOOP    Equal El1=RA_LOAD_BYTE El2=LF         Exit=LD_FLAG
ITO LOAD_IRA_LFJ     JumpIf El1=LD_FLAG    Exit=LOAD_IRA_DONE
JZ LOAD_IRA_EOFCK RA_LOAD_BYTE LOAD_IRA_DONE
RVOCA LOAD_IRA_TOK   BS_READ_TOKEN
JZ LOAD_IRA_EMCK RA_LOAD_TLEN LOAD_IRA_DONE
/Save key bytes BEFORE stripping: B1 and B2
ITO LOAD_IRA_KB1A    Add   El1=BS_TOKBUF_BASE El2=C_1        Exit=LD_TMP
ITO LOAD_IRA_KB1     Read  El1=LD_TMP      Exit=LD_IRA_KEY1
ITO LOAD_IRA_KB2A    Add   El1=BS_TOKBUF_BASE El2=C_2        Exit=LD_TMP
ITO LOAD_IRA_KB2     Read  El1=LD_TMP      Exit=LD_IRA_KEY2
/Strip key= and intern value
RVOCA LOAD_IRA_KV    BS_TOKEN_VALUE
RVOCA LOAD_IRA_VI    BS_INTERN
/Route by key: B1=l(108)→El*, B1=x(120)→Exit
ITO LOAD_IRA_CKL     Equal El1=LD_IRA_KEY1  El2=ASCII_ll   Exit=LD_FLAG
ITO LOAD_IRA_CKLJ    JumpIf El1=LD_FLAG     Exit=LOAD_IRA_EL
ITO LOAD_IRA_CKX     Equal El1=LD_IRA_KEY1  El2=ASCII_xl   Exit=LD_FLAG
ITO LOAD_IRA_CKXJ    JumpIf El1=LD_FLAG     Exit=LOAD_IRA_EXIT
ITO LOAD_IRA_CONT    Jump  Exit=LOAD_IRA_LOOP
/El: check digit B2
NOLINK
ITO LOAD_IRA_EL      Equal El1=LD_IRA_KEY2  El2=ASCII_1   Exit=LD_FLAG
ITO LOAD_IRA_ELJ     JumpIf El1=LD_FLAG     Exit=LOAD_IRA_E1
ITO LOAD_IRA_WE2     Move  El1=RA_BS_RESULT Exit=LD_ITO_E2
ITO LOAD_IRA_CE2     Jump  Exit=LOAD_IRA_LOOP
NOLINK
ITO LOAD_IRA_E1      Move  El1=RA_BS_RESULT Exit=LD_ITO_E1
ITO LOAD_IRA_CE1     Jump  Exit=LOAD_IRA_LOOP
/Exit:
NOLINK
ITO LOAD_IRA_EXIT    Move  El1=RA_BS_RESULT Exit=LD_ITO_EXIT
ITO LOAD_IRA_CEX     Jump  Exit=LOAD_IRA_LOOP
NOLINK
RREDI LOAD_IRA_DONE
/── LOAD_CMD_BLOCK: BLOCK name count ─────────────────────────
NOLINK
/Read name
RVOCA LOAD_CMD_BLOCK   BS_READ_TOKEN
RVOCA LOAD_CB_NINT   BS_INTERN
ITO LOAD_CB_NADDR    Move  El1=RA_BS_RESULT Exit=LD_TMP
/If block already exists (found in htable AND has non-zero word): skip alloc
JZ LOAD_CB_EXCK RA_BS_RESULT LOAD_CB_DONEW
ITO LOAD_CB_EXCRD    Read  El1=RA_BS_RESULT Exit=LD_FLAG
JZ LOAD_CB_EXCZ LD_FLAG LOAD_CB_SKLS
/Already allocated: skip to EOL
NOLINK
/Read count token
RVOCA LOAD_CB_DONEW   BS_READ_TOKEN
RVOCA LOAD_CB_CPINT  BS_PARSE_INT
/RA_BS_TMP2 = count. Alloc count*2 luces.
ITO LOAD_CB_MUL      Mul   El1=RA_BS_TMP2   El2=C_2           Exit=RA_ALLOC_COUNT
RVOCA LOAD_CB_ALLOC  ALLOC_LUCES
/Register: aether[name_lux] = first lux addr
ITO LOAD_CB_REG      Write El1=LD_TMP       El2=RA_ALLOC_RESULT
RVOCA LOAD_CB_SKLS   BS_SKIP_TO_EOL
RREDI LOAD_CB_RRET
/── LOAD_CMD_NEWREF: NEWREF name [ref] ────────────────────────
/Reads name token, allocs or finds data lux (with packed name via LOAD_CN_INTERN).
/If ref token follows: word(name) = addr(ref). Else: self-ref.
NOLINK
/Read name token — RA_LOAD_TLEN and RA_LOAD_HASH set by BS_READ_TOKEN
RVOCA LOAD_CMD_NEWREF   BS_READ_TOKEN
/Set LD_TMP=0 (no lumens for NEWREF luces)
CLEAR LOAD_CNR_SETL LD_TMP
/Intern name with packed name via LOAD_CN_INTERN
RVOCA LOAD_CNR_ICTX   LOAD_CN_INTERN
ITO LOAD_CNR_NADDR   Move  El1=RA_BS_RESULT  Exit=RA_BS_EL0
/Read ref token (may be absent)
RVOCA LOAD_CNR_RTOK  BS_READ_TOKEN
JZ LOAD_CNR_RLEN RA_LOAD_TLEN LOAD_CNR_SELF
/Has ref: look up ref data lux, set word(name)=addr(ref)
RVOCA LOAD_CNR_RLK   BS_LOOKUP
ITO LOAD_CNR_WR      Write El1=RA_BS_EL0   El2=RA_BS_RESULT
RVOCA LOAD_CNR_SKPL  BS_SKIP_TO_EOL
RREDI LOAD_CNR_RRET
/No ref: self-ref
NOLINK
ITO LOAD_CNR_SELF    Write El1=RA_BS_EL0   El2=RA_BS_EL0
RVOCA LOAD_CNR_SRSK  BS_SKIP_TO_EOL
RREDI LOAD_CNR_SRRET3
/── LOAD_CMD_NEWSET: NEWSET name value ────────────────────────
/Reads name token, reads value token (integer or symbol ref).
/Sets word(name) = integer value or addr(symbol).
/Automatically adds lumen (Constant → Yaku) to mark this lux as a compile-time constant.
/PRELOAD_CONST in yaku.re detects this and emits "or i{XLEN} 0, val" instead of a heap load.
NOLINK
/Read name token and look up its data lux
RVOCA LOAD_CMD_NEWSET   BS_READ_TOKEN
RVOCA LOAD_CNS_NLK   BS_LOOKUP
ITO LOAD_CNS_NADDR   Move  El1=RA_BS_RESULT  Exit=RA_BS_EL0
/Read value token
RVOCA LOAD_CNS_VTOK  BS_READ_TOKEN
/Try parse as integer (result in RA_BS_TMP2)
RVOCA LOAD_CNS_PI    BS_PARSE_INT
/Write integer value to word(name)
ITO LOAD_CNS_WR      Write El1=RA_BS_EL0   El2=RA_BS_TMP2
/Tag as Constant: ADD_LUMEN(name, Constant, Yaku)
ITO LOAD_CNS_LSRC    Move  El1=RA_BS_EL0   Exit=RA_LM_SRC
ITO LOAD_CNS_LREL    Move  El1=Constant    Exit=RA_LM_REL
ITO LOAD_CNS_LTGT    Move  El1=Yaku        Exit=RA_LM_EXIT
RVOCA LOAD_CNS_LAL   ADD_LUMEN
RVOCA LOAD_CNS_SKPL  BS_SKIP_TO_EOL
RREDI LOAD_CNS_RRET
/── LOAD_CMD_SETREF: SETREF name ref ──────────────────────────
/Reads name and ref tokens, sets word(name) = addr(ref).
NOLINK
RVOCA LOAD_CMD_SETREF   BS_READ_TOKEN
RVOCA LOAD_CSR_NLK   BS_LOOKUP
ITO LOAD_CSR_NADDR   Move  El1=RA_BS_RESULT  Exit=RA_BS_EL0
RVOCA LOAD_CSR_RTOK  BS_READ_TOKEN
RVOCA LOAD_CSR_RLK   BS_LOOKUP
ITO LOAD_CSR_WR      Write El1=RA_BS_EL0   El2=RA_BS_RESULT
RVOCA LOAD_CSR_SKPL  BS_SKIP_TO_EOL
RREDI LOAD_CSR_RRET
/── LOAD_CMD_RVOCA_IMPL: RVOCA name sub ───────────────────────
/Reads name and sub tokens. Wires ITO: word=self, op=Voca, e1=sub, exit=RA_LINK.
NOLINK
/Read name token and look up data lux
RVOCA LOAD_CMD_RVOCA_IMPL   BS_READ_TOKEN
RVOCA LOAD_CRV_NLK   BS_LOOKUP
ITO LOAD_CRV_NADDR   Move  El1=RA_BS_RESULT  Exit=RA_BS_EL0
/Read sub token and look up data lux
RVOCA LOAD_CRV_STOK  BS_READ_TOKEN
RVOCA LOAD_CRV_SLK   BS_LOOKUP
ITO LOAD_CRV_SADDR   Move  El1=RA_BS_RESULT  Exit=RA_BS_EL1
/Skip rest of line (comment)
RVOCA LOAD_CRV_SKP   BS_SKIP_TO_EOL
/Check name not 0
JZ LOAD_CRV_NCKZ RA_BS_EL0 LOAD_CRV_DONE
/Wire: word=self
ITO LOAD_CRV_SELF    Write El1=RA_BS_EL0    El2=RA_BS_EL0
/op=Voca (slot 1)
ITO LOAD_CRV_OPS     Add   El1=RA_BS_EL0    El2=C_1           Exit=LD_FLAG
ITO LOAD_CRV_OPW     Write El1=LD_FLAG        El2=Voca
/e1=sub (slot 2)
ITO LOAD_CRV_E1S     Add   El1=RA_BS_EL0    El2=C_2           Exit=LD_FLAG
ITO LOAD_CRV_E1W     Write El1=LD_FLAG        El2=RA_BS_EL1
/exit=RA_LINK (slot 4)
ITO LOAD_CRV_EXS     Add   El1=RA_BS_EL0    El2=C_4           Exit=LD_FLAG
ITO LOAD_CRV_EXW     Write El1=LD_FLAG        El2=RA_LINK_REF
/Autolink from RA_MC_PREV
JZ LOAD_CRV_ALCK RA_MC_PREV LOAD_CRV_SETPREV
ITO LOAD_CRV_ALS     Add   El1=RA_MC_PREV     El2=C_5           Exit=LD_FLAG
ITO LOAD_CRV_ALW     Write El1=LD_FLAG         El2=RA_BS_EL0
ITO LOAD_CRV_SETPREV Move  El1=RA_BS_EL0    Exit=RA_MC_PREV
RREDI LOAD_CRV_DONE
/── LOAD_CMD_RREDI_IMPL: RREDI name ───────────────────────────
/Reads name token. Wires ITO: word=self, op=Redi, e1=RA_LINK, exit=RA_LINK.
/Resets RA_MC_PREV to 0 (RREDI breaks autolink chain).
NOLINK
RVOCA LOAD_CMD_RREDI_IMPL   BS_READ_TOKEN
RVOCA LOAD_CRR_NLK   BS_LOOKUP
ITO LOAD_CRR_NADDR   Move  El1=RA_BS_RESULT  Exit=RA_BS_EL0
RVOCA LOAD_CRR_SKP   BS_SKIP_TO_EOL
JZ LOAD_CRR_NCKZ RA_BS_EL0 LOAD_CRR_DONE
ITO LOAD_CRR_SELF    Write El1=RA_BS_EL0    El2=RA_BS_EL0
ITO LOAD_CRR_OPS     Add   El1=RA_BS_EL0    El2=C_1           Exit=LD_FLAG
ITO LOAD_CRR_OPW     Write El1=LD_FLAG        El2=Redi
ITO LOAD_CRR_E1S     Add   El1=RA_BS_EL0    El2=C_2           Exit=LD_FLAG
ITO LOAD_CRR_E1W     Write El1=LD_FLAG        El2=RA_LINK_REF
ITO LOAD_CRR_EXS     Add   El1=RA_BS_EL0    El2=C_4           Exit=LD_FLAG
ITO LOAD_CRR_EXW     Write El1=LD_FLAG        El2=RA_LINK_REF
/Autolink then reset chain
JZ LOAD_CRR_ALCK RA_MC_PREV LOAD_CRR_CLRPREV
ITO LOAD_CRR_ALS     Add   El1=RA_MC_PREV     El2=C_5           Exit=LD_FLAG
ITO LOAD_CRR_ALW     Write El1=LD_FLAG         El2=RA_BS_EL0
CLEAR LOAD_CRR_CLRPREV RA_MC_PREV
RREDI LOAD_CRR_DONE
/── LOAD_MA_READARG: read one file token → LD_MA_RESULT (0=done) ─────
/OUT: LD_MA_RESULT = resolved addr (symbol or C_N), or 0 if EOL/comment.
/Also sets LD_MA_HASH = RA_LOAD_HASH after successful read (for _J/_K extension).
NEW LD_MA_RESULT
NEW LD_MA_HASH      /saved hash of most recently read token
NEW LD_MA_HASH0     /saved hash of MA0 token (for _J/_K name extension in JEQ_JK)
NEWREF LD_MA0_ADDR  RA_MA0  /stores addr of RA_MA0 for builder detection
NEWREF LD_ALLOC_ITO_OP __LT_ALLOC_ITO  /load-time alloc-ITO sentinel
NOLINK
/Check: if last byte was LF or EOF → line already consumed → return 0
ITO LOAD_MA_READARG    Equal El1=RA_LOAD_BYTE El2=LF         Exit=LD_FLAG
ITO LOAD_MA_PRELFJ   JumpIf El1=LD_FLAG     Exit=LOAD_MA_ZERO
JZ LOAD_MA_PREEOF RA_LOAD_BYTE LOAD_MA_ZERO
RVOCA LOAD_MA_TOK    BS_READ_TOKEN
/tlen==0 → EOL/EOF → return 0
JZ LOAD_MA_TLENCK RA_LOAD_TLEN LOAD_MA_ZERO
/Check first byte
ITO LOAD_MA_B0       Read  El1=BS_TOKBUF_BASE Exit=LD_MA_RESULT
/If '/' (47) → comment → skip rest of line → return 0
ITO LOAD_MA_SLCK     Equal El1=LD_MA_RESULT El2=SLASH      Exit=LD_FLAG
ITO LOAD_MA_SLCKJ    JumpIf El1=LD_FLAG     Exit=LOAD_MA_SKP
/Check digit: (byte - 48) < 10 → it's a digit
ITO LOAD_MA_DIGS     Sub   El1=LD_MA_RESULT El2=ASCII_0  Exit=LD_TMP
ITO LOAD_MA_DIGCK    ULess El1=LD_TMP       El2=C_10          Exit=LD_FLAG
ITO LOAD_MA_DIGCJ    JumpIf El1=LD_FLAG     Exit=LOAD_MA_INT
/Also handle '-' negative numbers
ITO LOAD_MA_NEGCK    Equal El1=LD_MA_RESULT El2=MINUS    Exit=LD_FLAG
ITO LOAD_MA_NEGCJ    JumpIf El1=LD_FLAG     Exit=LOAD_MA_INT
/Symbol: BS_LOOKUP
RVOCA LOAD_MA_LK     BS_LOOKUP
ITO LOAD_MA_LKRES    Move  El1=RA_BS_RESULT Exit=LD_MA_RESULT
ITO LOAD_MA_SVHASH   Move  El1=RA_LOAD_HASH Exit=LD_MA_HASH
RREDI LOAD_MA_RRET
/Integer: BS_PARSE_INT → C_N addr = C_0_lux + value * 2
NOLINK
RVOCA LOAD_MA_INT    BS_PARSE_INT
/C_0 lux = 746 (a[C_0]). Add El1=C_0 adds 746.
ITO LOAD_MA_C2       Mul   El1=RA_BS_TMP2   El2=C_2           Exit=LD_TMP
ITO LOAD_MA_CN       Add   El1=C_0          El2=LD_TMP        Exit=LD_MA_RESULT
ITO LOAD_MA_SVHI     Move  El1=RA_LOAD_HASH Exit=LD_MA_HASH
RREDI LOAD_MA_INTRR
/Comment: skip rest of line
NOLINK
RVOCA LOAD_MA_SKP    BS_SKIP_TO_EOL
CLEAR LOAD_MA_ZERO LD_MA_RESULT
RREDI LOAD_MA_ZRRET
/── LOAD_MA_SETUP: read file tokens → MA0..MA7 ───────────────────────
/Reads up to 8 positional tokens, resolves each, stores in RA_MA0..RA_MA7.
/After first successful read, saves RA_LOAD_HASH in LD_MA_HASH for _J/_K extension.
/Stops at EOL/EOF/comment. Clears remaining MA registers to 0.
NOLINK
/Clear MA0..MA7 before reading new args.
FOR RA_MA0 RA_MA1 RA_MA2 RA_MA3 RA_MA4 RA_MA5 RA_MA6 RA_MA7
    CLEAR LOAD_MAS_CLR{N} {X}
RVOCA LOAD_MA_SETUP      LOAD_MA_READARG
JZ LOAD_MAS_0_CK LD_MA_RESULT LOAD_MAS_RET
ITO LOAD_MAS_0_SET      Move  El1=LD_MA_RESULT Exit=RA_MA0
/Save hash(MA0) before reading MA1..MA7 overwrite it (JEQ_JK/JZ_JK need hash(MA0))
ITO LOAD_MAS_0_SH       Move  El1=LD_MA_HASH   Exit=LD_MA_HASH0
RVOCA LOAD_MAS_1_RD      LOAD_MA_READARG
JZ LOAD_MAS_1_CK LD_MA_RESULT LOAD_MAS_RET
ITO LOAD_MAS_1_SET      Move  El1=LD_MA_RESULT Exit=RA_MA1
RVOCA LOAD_MAS_2_RD      LOAD_MA_READARG
JZ LOAD_MAS_2_CK LD_MA_RESULT LOAD_MAS_RET
ITO LOAD_MAS_2_SET      Move  El1=LD_MA_RESULT Exit=RA_MA2
RVOCA LOAD_MAS_3_RD      LOAD_MA_READARG
JZ LOAD_MAS_3_CK LD_MA_RESULT LOAD_MAS_RET
ITO LOAD_MAS_3_SET      Move  El1=LD_MA_RESULT Exit=RA_MA3
RVOCA LOAD_MAS_4_RD      LOAD_MA_READARG
JZ LOAD_MAS_4_CK LD_MA_RESULT LOAD_MAS_RET
ITO LOAD_MAS_4_SET      Move  El1=LD_MA_RESULT Exit=RA_MA4
RVOCA LOAD_MAS_5_RD      LOAD_MA_READARG
JZ LOAD_MAS_5_CK LD_MA_RESULT LOAD_MAS_RET
ITO LOAD_MAS_5_SET      Move  El1=LD_MA_RESULT Exit=RA_MA5
RVOCA LOAD_MAS_6_RD      LOAD_MA_READARG
JZ LOAD_MAS_6_CK LD_MA_RESULT LOAD_MAS_RET
ITO LOAD_MAS_6_SET      Move  El1=LD_MA_RESULT Exit=RA_MA6
RVOCA LOAD_MAS_7_RD      LOAD_MA_READARG
JZ LOAD_MAS_7_CK LD_MA_RESULT LOAD_MAS_RET
ITO LOAD_MAS_7_SET      Move  El1=LD_MA_RESULT Exit=RA_MA7
RREDI LOAD_MAS_RET
/── LOAD_MA_JEQ_JK: auto-complete MA4/MA5 for JEQ from MA0 hash ──────
/If MA4==0: compute hash("MA0_name_J") = extend LD_MA_HASH with "_J" (95,74).
/If MA5==0: compute hash("MA0_name_K") = extend LD_MA_HASH with "_K" (95,75).
/BS_LOOKUP each computed hash → find existing _J/_K luces → store in MA4/MA5.
NOLINK
/Check if MA4 already set
JZ LOAD_MA_JEQ_JK RA_MA4 LOAD_MAJK_DO_J
/MA4 set, check MA5
JZ LOAD_MAJK_CK5 RA_MA5 LOAD_MAJK_DO_K
/Both set, done
RREDI LOAD_MAJK_RRET
/Compute hash for _J: (LD_MA_HASH * 33 + '_') * 33 + 'J'
CHAIN
    LOAD_MAJK_DO_J  Mul   El1=LD_MA_HASH0   El2=C_33       Exit=RA_BS_TMP2
    LOAD_MAJK_JU1   Add   El1=RA_BS_TMP2   El2=UNDERSCORE Exit=RA_BS_TMP2
    LOAD_MAJK_JU2   And   El1=RA_BS_TMP2   El2=BS_HT_MASK Exit=RA_BS_TMP2
    LOAD_MAJK_JJ1   Mul   El1=RA_BS_TMP2   El2=C_33       Exit=RA_LOAD_HASH
    LOAD_MAJK_JJ2   Add   El1=RA_LOAD_HASH El2=ASCII_J    Exit=RA_LOAD_HASH
    LOAD_MAJK_JJ3   And   El1=RA_LOAD_HASH El2=BS_HT_MASK Exit=RA_LOAD_HASH
        RVOCA LOAD_MAJK_JLK  BS_LOOKUP
    LOAD_MAJK_JSET  Move  El1=RA_BS_RESULT Exit=RA_MA4
        JZ LOAD_MAJK_JCK RA_MA4 LOAD_MAJK_JALLOC
    LOAD_MAJK_JFOUND Jump Exit=LOAD_MAJK_DO_K
NOLINK
ALLOC_TO LOAD_MAJK_JALLOC RA_MA4 ITO_SIZE
/Compute hash for _K: (LD_MA_HASH * 33 + '_') * 33 + 'K'
ITO LOAD_MAJK_DO_K   Mul   El1=LD_MA_HASH0   El2=C_33          Exit=RA_BS_TMP2
ITO LOAD_MAJK_KU1    Add   El1=RA_BS_TMP2   El2=UNDERSCORE Exit=RA_BS_TMP2
ITO LOAD_MAJK_KU2    And   El1=RA_BS_TMP2   El2=BS_HT_MASK    Exit=RA_BS_TMP2
ITO LOAD_MAJK_KK1    Mul   El1=RA_BS_TMP2   El2=C_33          Exit=RA_LOAD_HASH
ITO LOAD_MAJK_KK2    Add   El1=RA_LOAD_HASH El2=ASCII_K  Exit=RA_LOAD_HASH
ITO LOAD_MAJK_KK3    And   El1=RA_LOAD_HASH El2=BS_HT_MASK    Exit=RA_LOAD_HASH
RVOCA LOAD_MAJK_KLK  BS_LOOKUP
ITO LOAD_MAJK_KSET   Move  El1=RA_BS_RESULT Exit=RA_MA5
/Guard: if _K not found (MA5=0), alloc anonymous lux to prevent corrupt writes
JZ LOAD_MAJK_KCK RA_MA5 LOAD_MAJK_KALLOC
ITO LOAD_MAJK_KFOUND Jump  Exit=LOAD_MAJK_DONE
NOLINK
ALLOC_TO LOAD_MAJK_KALLOC RA_MA5 ITO_SIZE
RREDI LOAD_MAJK_DONE
/── Block comment handlers ─────────────────────────────────────
/LOAD_DL_BSLRD: read second byte to check for block-comment opener
NOLINK
RVOCA LOAD_DL_BSLB   BS_READ_BYTE
ITO LOAD_DL_BSLCK2   Equal El1=RA_LOAD_BYTE El2=SLASH     Exit=LD_FLAG
ITO LOAD_DL_BSLCJ2   JumpIf El1=LD_FLAG     Exit=LOAD_DL_BOPN
/Single slash: comment to end of line, skip
RVOCA LOAD_DL_BSLSK  BS_SKIP_TO_EOL
RREDI LOAD_DL_BSLSR2
/Open block comment: set LD_BLOCK_CMT=1, skip to EOL
NOLINK
ITO LOAD_DL_BOPN     Move  El1=C_1          Exit=LD_BLOCK_CMT
RVOCA LOAD_DL_BOPNSK BS_SKIP_TO_EOL
RREDI LOAD_DL_BOPNR2
/LOAD_DL_BCSKP: in block comment — scan line for closing double-slash
/peek byte is in LD_TMP; scan rest of line byte-by-byte for double-slash.
/Closing double-slash may appear anywhere: start, middle, or end of line.
NOLINK
ITO LOAD_DL_BCSKP    Equal  El1=LD_TMP      El2=SLASH     Exit=LD_FLAG
ITO LOAD_DL_BCSKPJ   JumpIf El1=LD_FLAG     Exit=LOAD_DL_BCSL2
/Line doesn't start with '/': scan from next byte
/LOAD_DL_BCSCAN: read next byte; search for double-slash
NOLINK
RVOCA LOAD_DL_BCSCAN  BS_READ_BYTE
JZ LOAD_DL_BCSEOF RA_LOAD_BYTE LOAD_DL_BCSRET
ITO LOAD_DL_BCSLF    Equal  El1=RA_LOAD_BYTE El2=LF       Exit=LD_FLAG
ITO LOAD_DL_BCSLFJ   JumpIf El1=LD_FLAG     Exit=LOAD_DL_BCSRET
ITO LOAD_DL_BCSSL    Equal  El1=RA_LOAD_BYTE El2=SLASH    Exit=LD_FLAG
ITO LOAD_DL_BCSSJ    JumpIf El1=LD_FLAG     Exit=LOAD_DL_BCFSL
ITO LOAD_DL_BCSNXT   Jump  Exit=LOAD_DL_BCSCAN
/Found first '/': read second byte
NOLINK
RVOCA LOAD_DL_BCFSL  BS_READ_BYTE
ITO LOAD_DL_BFSLCK   Equal  El1=RA_LOAD_BYTE El2=SLASH    Exit=LD_FLAG
ITO LOAD_DL_BFSLJMP  JumpIf El1=LD_FLAG     Exit=LOAD_DL_BCLS
JZ LOAD_DL_BFSLEOF RA_LOAD_BYTE LOAD_DL_BCSRET
ITO LOAD_DL_BFSLLF   Equal  El1=RA_LOAD_BYTE El2=LF       Exit=LD_FLAG
ITO LOAD_DL_BFSLLFJ  JumpIf El1=LD_FLAG     Exit=LOAD_DL_BCSRET
ITO LOAD_DL_BFSLNX   Jump  Exit=LOAD_DL_BCSCAN
/Line starts with '/': check if second byte is also '/'
NOLINK
RVOCA LOAD_DL_BCSL2  BS_READ_BYTE
ITO LOAD_DL_BCCK2    Equal El1=RA_LOAD_BYTE El2=SLASH     Exit=LD_FLAG
ITO LOAD_DL_BCCJ2    JumpIf El1=LD_FLAG     Exit=LOAD_DL_BCLS
/Second byte not slash — peek byte was '/' but alone; continue scanning
ITO LOAD_DL_BSL2NX   Jump  Exit=LOAD_DL_BCSCAN
/LOAD_DL_BCSRET: end of line reached, still in block comment
NOLINK
RREDI LOAD_DL_BCSRET
/Closing block comment: set LD_BLOCK_CMT=0, skip rest of line
NOLINK
CLEAR LOAD_DL_BCLS LD_BLOCK_CMT
RVOCA LOAD_DL_BCLSSK BS_SKIP_TO_EOL
RREDI LOAD_DL_BCLSR2
/── LOAD_CMD_NOLINK: suppress next autolink only (preserve BS_LAST_ITO)
/NOLINK means "next ITO does not autolink to previous". It does NOT break the
/tracked chain — BS_LAST_ITO keeps pointing to the last real ITO so that the
/ITO *after* the NOLINK block can still autolink correctly once enabled again.
/Only RA_MC_PREV is cleared (to 0) so the very next ITO skips autolink write.
NOLINK
CLEAR LOAD_CMD_NOLINK RA_MC_PREV
RVOCA LOAD_NL_SKP    BS_SKIP_TO_EOL
RREDI LOAD_NL_RET
/── LOAD_PREPASS_FILE: scan one file, count LINK src lumens ───
/Opens LD_FPATH, reads lines, for each LINK line: read src token,
/call LOAD_LCOUNT_INC to increment src's count in LD_LCOUNT_BUF.
/Skips all other lines. Closes file.
NOLINK
/Open file and init read buffer
RVOCA LOAD_PREPASS_FILE  LOAD_OPEN_FILE
ITO LOAD_PP_JMP2LP   Jump  Exit=LOAD_PP_LOOP
/Read loop: one line at a time
NOLINK
RVOCA LOAD_PP_LOOP   BS_READ_BYTE
/EOF check
JZ LOAD_PP_EOFCK RA_LOAD_BYTE LOAD_PP_CLOSE
/LF: empty line, continue
ITO LOAD_PP_LFCK     Equal El1=RA_LOAD_BYTE El2=LF         Exit=LD_FLAG
ITO LOAD_PP_LFJ      JumpIf El1=LD_FLAG     Exit=LOAD_PP_LOOP
/Comment '/': check single-slash or double-slash block comment
ITO LOAD_PP_SLCK     Equal El1=RA_LOAD_BYTE El2=SLASH      Exit=LD_FLAG
ITO LOAD_PP_SLJ      JumpIf El1=LD_FLAG     Exit=LOAD_PP_SL2
/Indented or space: skip line (not a command)
ITO LOAD_PP_SPCK     Equal El1=RA_LOAD_BYTE El2=SP      Exit=LD_FLAG
ITO LOAD_PP_SPCKJ    JumpIf El1=LD_FLAG     Exit=LOAD_PP_SKIP
ITO LOAD_PP_TBCK     Equal El1=RA_LOAD_BYTE El2=TAB        Exit=LD_FLAG
ITO LOAD_PP_TBCKJ    JumpIf El1=LD_FLAG     Exit=LOAD_PP_SKIP
/Prepend peeked byte to tokbuf, collect rest of token
/Write peeked byte (already in RA_LOAD_BYTE, saved to LD_TMP by earlier peek) to tokbuf[0]
/Note: RA_LOAD_BYTE still has the peeked byte at this point
ITO LOAD_PP_SVCB     Move  El1=RA_LOAD_BYTE  Exit=LD_TMP
ITO LOAD_PP_TB0      Move  El1=BS_TOKBUF_BASE Exit=RA_BS_TMP
ITO LOAD_PP_TBW      Write El1=RA_BS_TMP     El2=LD_TMP
ITO LOAD_PP_TINI     Move  El1=C_1           Exit=RA_LOAD_TLEN
ITO LOAD_PP_H0       Mul   El1=BS_HASH0      El2=C_33          Exit=RA_LOAD_HASH
ITO LOAD_PP_H1       Add   El1=RA_LOAD_HASH  El2=LD_TMP        Exit=RA_LOAD_HASH
ITO LOAD_PP_H2       And   El1=RA_LOAD_HASH  El2=BS_HT_MASK    Exit=RA_LOAD_HASH
RVOCA LOAD_PP_TOK    LOAD_DL_COLLECT
/After collect, tokbuf has full token. Check: is it "LINK"?
/Check tokbuf[0]='L'(76), tokbuf[1]='I'(73)
ITO LOAD_PP_B0       Read  El1=BS_TOKBUF_BASE Exit=LD_TMP
ITO LOAD_PP_LCK      Equal El1=LD_TMP        El2=ASCII_L Exit=LD_FLAG
ITO LOAD_PP_LCKJ     JumpIf El1=LD_FLAG      Exit=LOAD_PP_CHK2
ITO LOAD_PP_NOTL     Jump  Exit=LOAD_PP_SKIP
NOLINK
ITO LOAD_PP_CHK2     Add   El1=BS_TOKBUF_BASE El2=C_1         Exit=LD_TMP2
ITO LOAD_PP_B1       Read  El1=LD_TMP2        Exit=LD_TMP
ITO LOAD_PP_ICK      Equal El1=LD_TMP         El2=ASCII_I Exit=LD_FLAG
ITO LOAD_PP_ICKJ     JumpIf El1=LD_FLAG       Exit=LOAD_PP_LINK
ITO LOAD_PP_NOTL2    Jump  Exit=LOAD_PP_SKIP
/It's LINK: read src token and increment its count
NOLINK
RVOCA LOAD_PP_LINK   BS_READ_TOKEN
/Intern the token to get a unique address (collision-free key for LCOUNT)
RVOCA LOAD_PP_LINKI  BS_INTERN
/RA_BS_RESULT = unique lux addr for this name → use as key in LOAD_LCOUNT_INC
ITO LOAD_PP_LHK      Move  El1=RA_BS_RESULT Exit=RA_LOAD_HASH
/Call LOAD_LCOUNT_INC
RVOCA LOAD_PP_INC    LOAD_LCOUNT_INC
ITO LOAD_PP_IJMP     Jump  Exit=LOAD_PP_SKIP
/Skip rest of line
NOLINK
RVOCA LOAD_PP_SKIP   BS_SKIP_TO_EOL
ITO LOAD_PP_SJMP     Jump  Exit=LOAD_PP_LOOP
/SL2: second byte after '/' — block or single-line comment
NOLINK
RVOCA LOAD_PP_SL2   BS_READ_BYTE
ITO LOAD_PP_SL2CK    Equal El1=RA_LOAD_BYTE El2=SLASH      Exit=LD_FLAG
ITO LOAD_PP_SL2J     JumpIf El1=LD_FLAG     Exit=LOAD_PP_BLOCK
/Single '/' → skip to EOL
RVOCA LOAD_PP_SL2SK  BS_SKIP_TO_EOL
ITO LOAD_PP_SL2JMP   Jump  Exit=LOAD_PP_LOOP
/Block comment: scan until closing double-slash
NOLINK
RVOCA LOAD_PP_BLOCK   BS_READ_BYTE
JZ LOAD_PP_BEOFCK RA_LOAD_BYTE LOAD_PP_CLOSE
ITO LOAD_PP_BSLCK    Equal El1=RA_LOAD_BYTE El2=SLASH      Exit=LD_FLAG
ITO LOAD_PP_BSLJ     JumpIf El1=LD_FLAG     Exit=LOAD_PP_BSL2
ITO LOAD_PP_BJMP     Jump  Exit=LOAD_PP_BLOCK
NOLINK
RVOCA LOAD_PP_BSL2   BS_READ_BYTE
JZ LOAD_PP_BSL2EOF RA_LOAD_BYTE LOAD_PP_CLOSE
ITO LOAD_PP_BSL2CK   Equal El1=RA_LOAD_BYTE El2=SLASH      Exit=LD_FLAG
ITO LOAD_PP_BSL2J    JumpIf El1=LD_FLAG     Exit=LOAD_PP_BEND
ITO LOAD_PP_BSL2NKJ  JumpIf El1=LD_FLAG     Exit=LOAD_PP_BSL2
ITO LOAD_PP_BSL2NJP  Jump  Exit=LOAD_PP_BLOCK
NOLINK
RVOCA LOAD_PP_BEND   BS_SKIP_TO_EOL
ITO LOAD_PP_BJMP2    Jump  Exit=LOAD_PP_LOOP
/Close file
NOLINK
ITO LOAD_PP_CLOSE    Move  El1=SYS_CLOSE    Exit=SC_NR
ITO LOAD_PP_CLFD     Move  El1=RA_LOAD_FD   Exit=SC_A0
ITO LOAD_PP_CLXR     Exire El1=C_0          El2=C_0            Exit=C_0
RREDI LOAD_PP_RRET
/── LOAD_LCOUNT_INC: increment link count for src name ────────
/IN: RA_LOAD_HASH = intern addr of src name (unique, collision-free via BS_INTERN)
/Walks LD_LCOUNT_BUF pairs [addr, count] looking for matching addr.
/On miss: creates new pair at LD_LCOUNT_PTR.
NOLINK
ITO LOAD_LCOUNT_INC     Move  El1=LD_LCOUNT_BASE Exit=LD_TMP
/Linear scan: look for matching hash
ITO LOAD_LCI_LOOP    Read  El1=LD_TMP        Exit=LD_TMP2
/0 = end of table → not found
JZ LOAD_LCI_ZCKL LD_TMP2 LOAD_LCI_NEW
/Match?
ITO LOAD_LCI_MCK     Equal El1=LD_TMP2       El2=RA_LOAD_HASH  Exit=LD_FLAG
ITO LOAD_LCI_MCKJ    JumpIf El1=LD_FLAG      Exit=LOAD_LCI_HIT
/Advance by 2 (skip pair)
ITO LOAD_LCI_ADV     Add   El1=LD_TMP        El2=C_2           Exit=LD_TMP
ITO LOAD_LCI_JMP     Jump  Exit=LOAD_LCI_LOOP
/Hit: increment count at LD_TMP+1
NOLINK
ITO LOAD_LCI_HIT     Add   El1=LD_TMP        El2=C_1           Exit=LD_TMP2
ITO LOAD_LCI_RD      Read  El1=LD_TMP2       Exit=LD_FLAG
ITO LOAD_LCI_INC     Add   El1=LD_FLAG       El2=C_1           Exit=LD_FLAG
ITO LOAD_LCI_WR      Write El1=LD_TMP2       El2=LD_FLAG
RREDI LOAD_LCI_RRET
/New entry: write (hash, 1) at LD_LCOUNT_PTR
CHAIN
    LOAD_LCI_NEW  Write El1=LD_LCOUNT_PTR El2=RA_LOAD_HASH
    LOAD_LCI_INC2 Add   El1=LD_LCOUNT_PTR El2=C_1           Exit=LD_TMP2
    LOAD_LCI_WC   Write El1=LD_TMP2       El2=C_1
    LOAD_LCI_ADV2 Add   El1=LD_LCOUNT_PTR El2=C_2           Exit=LD_LCOUNT_PTR
    LOAD_LCI_SEN  Write El1=LD_LCOUNT_PTR El2=C_0
        RREDI LOAD_LCI_NRRET

/── LOAD_LCOUNT_GET: get link count for name hash → LD_TMP ────
/IN: RA_LOAD_HASH = hash of name
/OUT: LD_TMP = count (0 if not found)
NOLINK
ITO LOAD_LCOUNT_GET     Move  El1=LD_LCOUNT_BASE Exit=LD_TMP2
ITO LOAD_LCG_LOOP    Read  El1=LD_TMP2      Exit=LD_TMP
JZ LOAD_LCG_ZCKL LD_TMP LOAD_LCG_MISS
ITO LOAD_LCG_MCK     Equal El1=LD_TMP       El2=RA_LOAD_HASH  Exit=LD_FLAG
ITO LOAD_LCG_MCKJ    JumpIf El1=LD_FLAG     Exit=LOAD_LCG_HIT
ITO LOAD_LCG_ADV     Add   El1=LD_TMP2      El2=C_2           Exit=LD_TMP2
ITO LOAD_LCG_JMP     Jump  Exit=LOAD_LCG_LOOP
NOLINK
ITO LOAD_LCG_HIT     Add   El1=LD_TMP2      El2=C_1           Exit=LD_TMP2
ITO LOAD_LCG_RD      Read  El1=LD_TMP2      Exit=LD_TMP
RREDI LOAD_LCG_RRET
NOLINK
CLEAR LOAD_LCG_MISS LD_TMP
RREDI LOAD_LCG_MRRET
/── LOAD_CMD_UNKNOWN: lookup CMD in htable → call program ─────
/LD_CMD_ADDR = addr of cmd lux (already interned). Read els into MA*.
/MA0 = cmd lux addr (always set). Els read before dispatch.
/If program addr = 0: command undefined → skip rest of line, return.
/If program addr ≠ 0: call it. This is the single dispatch rule.
NOLINK
/aether[LD_CMD_ADDR] = entry point of handler program
ITO LOAD_CMD_UNKNOWN   Read  El1=LD_CMD_ADDR  Exit=LD_TMP
/If 0: undefined command — skip rest of line
JZ LOAD_CU_CK LD_TMP LOAD_CU_SKPL
/Detect builder macro: if handler's first ITO e1 == RA_MA0 → needs MA* setup from file.
/No NEXT==0 guard here — builder macros like RCALL_AT_SPN are NOLINK (NEXT=0) by design.
/Detect builder macro: if handler's first ITO Exit == RA_MC_SLOT → writes a slot (builder chain).
/This correctly identifies LH (exit=RA_MC_SLOT via RA_MA5) and all N?_W0 builder ITOs.
/Native handlers (RVOCA_IMPL, RREDI_IMPL) use Exit=RA_LINK, not RA_MC_SLOT.
ITO LOAD_CU_BME1S    Add   El1=LD_TMP       El2=C_4           Exit=LD_FLAG
ITO LOAD_CU_BME1R    Read  El1=LD_FLAG      Exit=LD_FLAG
ITO LOAD_CU_BMCK     Equal El1=LD_FLAG      El2=RA_LINK_REF   Exit=LD_FLAG
ITO LOAD_CU_BMCJ     JumpIf El1=LD_FLAG     Exit=LOAD_CU_PREV
ITO LOAD_CU_BMBLD    Jump                   Exit=LOAD_CU_BUILDER
/Native handler: reads its own els from file
ITO LOAD_CU_PREV     Move  El1=BS_LAST_ITO  Exit=RA_MC_PREV
ITO LOAD_CU_MARET    Move  El1=RA_MC_PREV   Exit=RA_MA_RET
ITO LOAD_CU_CALL     Voca  El1=LD_TMP       Exit=RA_LINK
ITO LOAD_CU_SYNC     Move  El1=RA_MC_PREV   Exit=BS_LAST_ITO
RREDI LOAD_CU_DRET
/Builder macro path: read els from file → MA*, then call
NOLINK
ITO LOAD_CU_BUILDER  Move  El1=LD_TMP       Exit=LD_TMP2
RVOCA LOAD_CU_MAS    LOAD_MA_SETUP
/Guard: if MA0=0 (lux name not in htable), skip macro call entirely.
/This is the only needed safety check — macros with 1..3 args are valid (FUNC, RVOCA, RCALL_AT).
JZ LOAD_CU_MA0CK RA_MA0 LOAD_CU_SKPL
/Auto-complete _J/_K luces only for JEQ and JZ macros.
/JZ uses MA3/MA4, JEQ uses MA4/MA5. Other macros: proceed directly.
ITO LOAD_CU_JZCK     Equal El1=LD_TMP2       El2=JZ            Exit=LD_FLAG
ITO LOAD_CU_JZCKJ    JumpIf El1=LD_FLAG      Exit=LOAD_CU_JZ_PATH
/Check if JEQ
ITO LOAD_CU_JEQCK    Equal El1=LD_TMP2       El2=JEQ           Exit=LD_FLAG
ITO LOAD_CU_JEQCKJ   JumpIf El1=LD_FLAG      Exit=LOAD_CU_JEQ_PATH
/Neither JEQ nor JZ: proceed directly — MA0≠0 is sufficient guard for all other macros.
ITO LOAD_CU_JK_SKIP  Jump  Exit=LOAD_CU_BPREV
/JEQ path
NOLINK
RVOCA LOAD_CU_JEQ_PATH   LOAD_MA_JEQ_JK
ITO LOAD_CU_JK_DONE  Jump  Exit=LOAD_CU_BPREV
/JZ path
NOLINK
RVOCA LOAD_CU_JZ_PATH   LOAD_MA_JZ_JK
/Set RA_MC_PREV and RA_MA_RET for autolink
ITO LOAD_CU_BPREV    Move  El1=BS_LAST_ITO  Exit=RA_MC_PREV
ITO LOAD_CU_BMAR     Move  El1=RA_MC_PREV   Exit=RA_MA_RET
/Call builder macro
ITO LOAD_CU_BCALL    Voca  El1=LD_TMP2      Exit=RA_LINK
ITO LOAD_CU_BSYNC    Move  El1=RA_MA_RET   Exit=BS_LAST_ITO  /restore pre-call BS_LAST_ITO (macro internals should not be in main chain)
RREDI LOAD_CU_BRET
/Unknown command: skip rest of line and continue
NOLINK
RVOCA LOAD_CU_SKPL   BS_SKIP_TO_EOL
RREDI LOAD_CU_SKRRET
/── LOAD_MA_JZ_JK: auto-complete MA3/MA4 for JZ from MA0 hash ────────────────
/JZ macro layout: MA3=_J (JumpIf), MA4=_K (NOP). Distinct from JEQ (MA4/MA5).
/If MA3==0: hash(MA0_name + "_J") → lookup → if found: MA3=result; else alloc.
/If MA4==0: hash(MA0_name + "_K") → lookup → if found: MA4=result; else alloc.
NOLINK
/Check MA3: if already set, skip _J lookup
JZ LOAD_MA_JZ_JK RA_MA3 LOAD_MZJK_JLK_GO
ITO LOAD_MZJK_JFOUND Jump  Exit=LOAD_MZJK_DO_K
/Compute hash for _J: extend LD_MA_HASH with "_J" (95=_, 74=J)
NOLINK
ITO LOAD_MZJK_JLK_GO Mul  El1=LD_MA_HASH0 El2=C_33 Exit=RA_LOAD_HASH
ITO LOAD_MZJK_JU1    Add  El1=RA_LOAD_HASH El2=UNDERSCORE Exit=RA_LOAD_HASH
ITO LOAD_MZJK_JU2    And  El1=RA_LOAD_HASH El2=BS_HT_MASK Exit=RA_LOAD_HASH
ITO LOAD_MZJK_JK1    Mul  El1=RA_LOAD_HASH El2=C_33 Exit=RA_LOAD_HASH
ITO LOAD_MZJK_JK2    Add  El1=RA_LOAD_HASH El2=ASCII_J Exit=RA_LOAD_HASH
ITO LOAD_MZJK_JK3    And  El1=RA_LOAD_HASH El2=BS_HT_MASK Exit=RA_LOAD_HASH
RVOCA LOAD_MZJK_JLK  BS_LOOKUP
ITO LOAD_MZJK_JSET   Move  El1=RA_BS_RESULT Exit=RA_MA3
/If not found (MA3=0): alloc ITO_SIZE luces as placeholder
JZ LOAD_MZJK_JFCK RA_MA3 LOAD_MZJK_JALLOC
ITO LOAD_MZJK_JOK    Jump  Exit=LOAD_MZJK_DO_K
NOLINK
ALLOC_TO LOAD_MZJK_JALLOC RA_MA3 ITO_SIZE
/Compute hash for _K: extend LD_MA_HASH with "_K" (95=_, 75=K)
ITO LOAD_MZJK_DO_K   Mul  El1=LD_MA_HASH0 El2=C_33 Exit=RA_BS_TMP2
ITO LOAD_MZJK_KU1    Add  El1=RA_BS_TMP2 El2=UNDERSCORE Exit=RA_BS_TMP2
ITO LOAD_MZJK_KU2    And  El1=RA_BS_TMP2 El2=BS_HT_MASK Exit=RA_BS_TMP2
ITO LOAD_MZJK_KK1    Mul  El1=RA_BS_TMP2 El2=C_33 Exit=RA_LOAD_HASH
ITO LOAD_MZJK_KK2    Add  El1=RA_LOAD_HASH El2=ASCII_K Exit=RA_LOAD_HASH
ITO LOAD_MZJK_KK3    And  El1=RA_LOAD_HASH El2=BS_HT_MASK Exit=RA_LOAD_HASH
RVOCA LOAD_MZJK_KLK  BS_LOOKUP
ITO LOAD_MZJK_KSET   Move  El1=RA_BS_RESULT Exit=RA_MA4
JZ LOAD_MZJK_KCK RA_MA4 LOAD_MZJK_KALLOC
ITO LOAD_MZJK_KFOUND Jump  Exit=LOAD_MZJK_DONE
NOLINK
ALLOC_TO LOAD_MZJK_KALLOC RA_MA4 ITO_SIZE
RREDI LOAD_MZJK_DONE
/── LOAD_READ_LINE: implement macros.re stub ──────────────────
/Reads one full line into token buffer area.
/Sets RA_LOAD_BYTE to first non-space byte, RA_LOAD_INDENT=0/1.
/This OVERWRITES the stub defined in macros.re by having the same
/NEWREF name pointing to this implementation.
NEWREF LOAD_READ_LINE LOAD_RL_IMPL
NOLINK
/Skip leading spaces, count indent depth (0=none, 1=one level, 2=two+ levels)
CLEAR LOAD_RL_IMPL LD_INDENT_DEPTH
RVOCA LOAD_RL_RB     BS_READ_BYTE
JZ LOAD_RL_EOFCK RA_LOAD_BYTE LOAD_RL_DONE
SWITCH RA_LOAD_BYTE
    SP  LOAD_RL_INDENTED
    TAB LOAD_RL_INDENTED
RREDI LOAD_RL_DONE
/First indent char seen: set depth=1, read next byte
NOLINK
ITO LOAD_RL_INDENTED Move  El1=C_1          Exit=LD_INDENT_DEPTH
RVOCA LOAD_RL_RB2    BS_READ_BYTE
JZ LOAD_RL_EOF2 RA_LOAD_BYTE LOAD_RL_INDDONE
SWITCH RA_LOAD_BYTE
    SP  LOAD_RL_INDENTED2
    TAB LOAD_RL_INDENTED2
ITO LOAD_RL_INDDONE2 Jump Exit=LOAD_RL_INDDONE
/Second indent char seen: depth=2, read next byte (skip remaining indent)
NOLINK
ITO LOAD_RL_INDENTED2 Move El1=C_2         Exit=LD_INDENT_DEPTH
RVOCA LOAD_RL_RB3    BS_READ_BYTE
/Skip any further indent chars to get to actual content
SWITCH RA_LOAD_BYTE
    SP  LOAD_RL_SKP3
    TAB LOAD_RL_SKP3
ITO LOAD_RL_INDDONE3 Jump Exit=LOAD_RL_INDDONE
NOLINK
RVOCA LOAD_RL_SKP3   BS_READ_BYTE
SWITCH RA_LOAD_BYTE
    SP  LOAD_RL_SKP3
    TAB LOAD_RL_SKP3
ITO LOAD_RL_SKP3_END Jump Exit=LOAD_RL_INDDONE
NOLINK
ITO LOAD_RL_EOF2     Jump Exit=LOAD_RL_INDDONE
NOLINK
RREDI LOAD_RL_INDDONE
/── LOAD_READ_TOKEN: forward to BS_READ_TOKEN ─────────────────
NEWREF LOAD_READ_TOKEN LOAD_RT_IMPL
NOLINK
RVOCA LOAD_RT_IMPL   BS_READ_TOKEN
RREDI LOAD_RT_RRET
/── LOAD_INTERN_TOKEN: intern current token → RA_LOAD_RESULT ──
NEWREF LOAD_INTERN_TOKEN LOAD_IT_IMPL
NOLINK
RVOCA LOAD_IT_IMPL    BS_INTERN
ITO LOAD_IT_RES      Move  El1=RA_BS_RESULT Exit=RA_LOAD_RESULT
RREDI LOAD_IT_RRET
/── LOAD_READ_BODY: read indented lines into body buffer ──────
/Reads lines while indented, stores token stream in LD_BODY_BUF.
/Each line is prefixed with its indent depth byte (1 or 2) as a marker.
/Stops at non-indented line or EOF (that line is NOT consumed).
NEWREF LOAD_READ_BODY LOAD_RB_IMPL
NOLINK
ITO LOAD_RB_IMPL     Move  El1=LD_BODY_BUF_BASE_VAL Exit=LD_BODY_PTR
CLEAR LOAD_RB_LC LD_BODY_LINE_CNT
/Peek next line
RVOCA LOAD_RB_RL     LOAD_READ_LINE
/If not indented or EOF: done
JZ LOAD_RB_EOFCK RA_LOAD_BYTE LOAD_RB_DONE
JZ LOAD_RB_INDCK LD_INDENT_DEPTH LOAD_RB_DONE
/Bounds check: if PTR >= LIMIT, skip write to prevent overflow
JEQ LOAD_RB_OVFLCK LD_BODY_PTR LD_BODY_BUF_LIMIT LOAD_RB_RL
/Write indent depth marker byte first (1 or 2)
ITO LOAD_RB_MARK     Write El1=LD_BODY_PTR  El2=LD_INDENT_DEPTH
ITO LOAD_RB_MINC     Add   El1=LD_BODY_PTR  El2=C_1           Exit=LD_BODY_PTR
/Store first content byte
ITO LOAD_RB_STORE    Write El1=LD_BODY_PTR  El2=RA_LOAD_BYTE
ITO LOAD_RB_INC      Add   El1=LD_BODY_PTR  El2=C_1           Exit=LD_BODY_PTR
ITO LOAD_RB_LINC     Add   El1=LD_BODY_LINE_CNT El2=C_1       Exit=LD_BODY_LINE_CNT
ITO LOAD_RB_JMP      Jump  Exit=LOAD_RB_RL
NOLINK
RREDI LOAD_RB_DONE
/── LOAD_EXPAND_TEMPLATE: expand body for current FOR element ──
/Walks LD_BODY_BUF bytes. Substitutes:
/  {X} → RA_FOR_ELEM string (name of current element, as token bytes)
/  {N} → decimal digits of RA_FOR_IDX
/For each line: copies to BS_TOKBUF_BASE for dispatch, then calls
/LOAD_DISPATCH_LINE-like processing.
/
/Body buffer contains raw bytes. Lines separated by LF.
/Substitution: when '{' seen, read until '}', check name:
/  "X" → copy element name bytes from LD_FOR_ELEM_NAME buffer
/  "N" → write decimal digits of RA_FOR_IDX
NEWREF LOAD_EXPAND_TEMPLATE LOAD_ET_IMPL
NEW LD_ET_PTR           /read position in body buf
NEW LD_ET_OUT_PTR       /write position in output (BS_TOKBUF_BASE reused)
NEW LD_ET_PH_IDX        /placeholder name scan index
NEW LD_ET_PH_BUF        /placeholder name buffer (small, reuse RA_BS_TMP*)
NEW LD_FOR_ELEM_NAME    /packed addr of element name string in htable
NOLINK
ITO LOAD_ET_IMPL     Move  El1=LD_BODY_BUF_BASE_VAL Exit=LD_ET_PTR
/Walk body buf until ptr >= body_ptr (end)
ITO LOAD_ET_LINE     Move  El1=BS_TOKBUF_BASE Exit=LD_ET_OUT_PTR
ITO LOAD_ET_LOOP     Equal El1=LD_ET_PTR    El2=LD_BODY_PTR   Exit=LD_FLAG
ITO LOAD_ET_LCKJ     JumpIf El1=LD_FLAG     Exit=LOAD_ET_FLUSH
ITO LOAD_ET_RBYTE    Read  El1=LD_ET_PTR    Exit=LD_TMP
ITO LOAD_ET_INC      Add   El1=LD_ET_PTR    El2=C_1           Exit=LD_ET_PTR
/Skip indent marker bytes (C_1=0x01, C_2=0x02) written by LOAD_READ_BODY
JEQ LOAD_ET_MRK1     LD_TMP C_1 LOAD_ET_LOOP
JEQ LOAD_ET_MRK2     LD_TMP C_2 LOAD_ET_LOOP
/LF → flush line as tokens and dispatch
ITO LOAD_ET_LFCK     Equal El1=LD_TMP       El2=LF         Exit=LD_FLAG
ITO LOAD_ET_LFJ      JumpIf El1=LD_FLAG     Exit=LOAD_ET_FLUSH_DISPATCH
/'{' → placeholder
ITO LOAD_ET_LBCK     Equal El1=LD_TMP       El2=LBRACE   Exit=LD_FLAG
ITO LOAD_ET_LBCJ     JumpIf El1=LD_FLAG     Exit=LOAD_ET_PLACEHOLDER
/Ordinary byte: write to output
ITO LOAD_ET_WBYTE    Write El1=LD_ET_OUT_PTR El2=LD_TMP
ITO LOAD_ET_OINC     Add   El1=LD_ET_OUT_PTR El2=C_1          Exit=LD_ET_OUT_PTR
ITO LOAD_ET_JMP      Jump  Exit=LOAD_ET_LOOP
/Placeholder: read name until '}', then substitute
NOLINK
CLEAR LOAD_ET_PLACEHOLDER LD_ET_PH_IDX
ITO LOAD_ET_PH_RD    Read  El1=LD_ET_PTR    Exit=LD_TMP
ITO LOAD_ET_PH_INC   Add   El1=LD_ET_PTR   El2=C_1           Exit=LD_ET_PTR
ITO LOAD_ET_PH_RBCK  Equal El1=LD_TMP      El2=RBRACE   Exit=LD_FLAG
ITO LOAD_ET_PH_RBJ   JumpIf El1=LD_FLAG    Exit=LOAD_ET_PH_DISPATCH
/Accumulate placeholder name byte
ITO LOAD_ET_PH_STORE Add   El1=LD_ET_PH_IDX El2=LD_ET_PH_IDX Exit=LD_ET_PH_BUF
ITO LOAD_ET_PH_SW    Add   El1=BS_TOKBUF_BASE El2=LD_ET_PH_IDX Exit=LD_FLAG
/Reuse RA_BS_TMP as temp name buf — store at high offset
ITO LOAD_ET_PH_SXXX  Write El1=LD_FLAG     El2=LD_TMP
ITO LOAD_ET_PH_IINC  Add   El1=LD_ET_PH_IDX El2=C_1          Exit=LD_ET_PH_IDX
ITO LOAD_ET_PH_JMP   Jump  Exit=LOAD_ET_PH_RD
/Dispatch placeholder: check first byte
NOLINK
ITO LOAD_ET_PH_DISPATCH Move El1=BS_TOKBUF_BASE Exit=LD_FLAG
ITO LOAD_ET_PH_RB0   Read  El1=LD_FLAG      Exit=LD_TMP
/X → element name
ITO LOAD_ET_PH_XCK   Equal El1=LD_TMP       El2=ASCII_X    Exit=LD_FLAG
ITO LOAD_ET_PH_XJ    JumpIf El1=LD_FLAG     Exit=LOAD_ET_EMIT_ELEM
/N → index as decimal
ITO LOAD_ET_PH_NCK   Equal El1=LD_TMP       El2=ASCII_N    Exit=LD_FLAG
ITO LOAD_ET_PH_NJ    JumpIf El1=LD_FLAG     Exit=LOAD_ET_EMIT_IDX
/Unknown placeholder: emit '{' + name + '}'  (passthrough)
ITO LOAD_ET_PH_UNK   Move  El1=LBRACE  Exit=LD_TMP
ITO LOAD_ET_PH_UW    Write El1=LD_ET_OUT_PTR El2=LD_TMP
ITO LOAD_ET_PH_UI    Add   El1=LD_ET_OUT_PTR El2=C_1          Exit=LD_ET_OUT_PTR
ITO LOAD_ET_PH_UJMP  Jump  Exit=LOAD_ET_LOOP
/Emit element name: read from FOR_IRIS_BUF at offset FOR_IRIS_IDX[RA_FOR_IDX].
/RA_FOR_IDX = current iteration (0-based). Name stored as NUL-terminated bytes.
NOLINK
ITO LOAD_ET_EMIT_ELEM Move  El1=RA_FOR_IDX    Exit=LD_TMP
/Get offset: aether[RA_FOR_IRIS_IDX_BASE + RA_FOR_IDX]
ITO LOAD_ET_EE_IDXA   Add   El1=RA_FOR_IRIS_IDX_BASE El2=LD_TMP Exit=LD_TMP2
ITO LOAD_ET_EE_OFFSET Read  El1=LD_TMP2        Exit=LD_TMP
/Name base = RA_FOR_IRIS_BUF + offset
ITO LOAD_ET_EE_NBASE  Add   El1=RA_FOR_IRIS_BUF El2=LD_TMP    Exit=LD_TMP2
/Emit name bytes until NUL
ITO LOAD_ET_EE_LOOP   Read  El1=LD_TMP2        Exit=LD_TMP
JZ LOAD_ET_EE_NULCK LD_TMP LOAD_ET_EE_DONE
ITO LOAD_ET_EE_WRITE  Write El1=LD_ET_OUT_PTR    El2=LD_TMP
ITO LOAD_ET_EE_OI     Add   El1=LD_ET_OUT_PTR    El2=C_1      Exit=LD_ET_OUT_PTR
ITO LOAD_ET_EE_NI     Add   El1=LD_TMP2          El2=C_1      Exit=LD_TMP2
ITO LOAD_ET_EE_JMP2   Jump  Exit=LOAD_ET_EE_LOOP
NOLINK
ITO LOAD_ET_EE_DONE   Jump  Exit=LOAD_ET_LOOP
/Emit index as decimal
NOLINK
ITO LOAD_ET_EMIT_IDX Move  El1=RA_FOR_IDX   Exit=LD_TMP
RVOCA LOAD_ET_EI_INT LOAD_EMIT_INT_BYTES
ITO LOAD_ET_EI_JMP   Jump  Exit=LOAD_ET_LOOP
/Flush line: write LF terminator, set RA_LOAD_TLEN, call dispatcher
NOLINK
/Fall through to dispatch
ITO LOAD_ET_FLUSH   Jump  Exit=LOAD_ET_FLUSH_DISPATCH
NOLINK
/Line is in BS_TOKBUF_BASE[0..out_ptr-base]. Set up for dispatch.
/Store LF at end for token reader
ITO LOAD_ET_FLUSH_DISPATCH   Write El1=LD_ET_OUT_PTR El2=LF
/Advance out_ptr past LF for length calculation
ITO LOAD_ET_FD_INC   Add   El1=LD_ET_OUT_PTR El2=C_1 Exit=LD_ET_OUT_PTR
/Set RA_LOAD_BYTE to first byte of expanded line
ITO LOAD_ET_FD_FB    Read  El1=BS_TOKBUF_BASE Exit=RA_LOAD_BYTE
/Copy expanded line from BS_TOKBUF_BASE into BS_READBUF_BASE so
/BS_READ_BYTE reads it correctly. Compute length = out_ptr - tokbuf_base.
ITO LOAD_ET_FD_LEN   Sub   El1=LD_ET_OUT_PTR  El2=BS_TOKBUF_BASE Exit=RA_LOAD_RLEN
CLEAR LOAD_ET_FD_RST RA_LOAD_RPOS  /reset file read pos to start of expanded line
/Copy loop: BS_READBUF_BASE[0..len-1] = BS_TOKBUF_BASE[0..len-1]
CLEAR LOAD_ET_FD_CPINIT LD_TMP
ITO LOAD_ET_FD_CPCK  Equal El1=LD_TMP El2=RA_LOAD_RLEN Exit=LD_FLAG
ITO LOAD_ET_FD_CPCKJ JumpIf El1=LD_FLAG Exit=LOAD_ET_FD_CPDONE
ITO LOAD_ET_FD_SRC   Add   El1=BS_TOKBUF_BASE El2=LD_TMP Exit=LD_TMP2
ITO LOAD_ET_FD_BYTE  Read  El1=LD_TMP2         Exit=LD_FLAG
ITO LOAD_ET_FD_DST   Add   El1=BS_READBUF_BASE El2=LD_TMP Exit=LD_TMP2
ITO LOAD_ET_FD_WR    Write El1=LD_TMP2         El2=LD_FLAG
ITO LOAD_ET_FD_INC2  Add   El1=LD_TMP El2=C_1 Exit=LD_TMP
ITO LOAD_ET_FD_CPJMP Jump  Exit=LOAD_ET_FD_CPCK
/Now BS_READBUF_BASE[0..RLEN-1] has expanded line; RPOS=0
NOLINK
RVOCA LOAD_ET_FD_CPDONE   LOAD_DISPATCH_LINE
ITO LOAD_ET_FD_RESET Move  El1=BS_TOKBUF_BASE Exit=LD_ET_OUT_PTR
ITO LOAD_ET_FD_JMP   Jump  Exit=LOAD_ET_LOOP
NOLINK
RREDI LOAD_ET_DRET
/── LOAD_EMIT_INT_BYTES: write decimal digits of LD_TMP to LD_ET_OUT_PTR ──
/Simple: handles 0-9999. Enough for iteration indices.
NEWREF LOAD_EMIT_INT_BYTES LOAD_EIB_IMPL
NEW LD_EIB_VAL
NEW LD_EIB_DIV
NOLINK
ITO LOAD_EIB_IMPL    Move  El1=LD_TMP       Exit=LD_EIB_VAL
/Divide by 1000, 100, 10, 1 and emit non-zero digits
/Thousands
ITO LOAD_EIB_D1000   Div   El1=LD_EIB_VAL   El2=C_100         Exit=LD_EIB_DIV
ITO LOAD_EIB_R1000   Rem   El1=LD_EIB_VAL   El2=C_100         Exit=LD_EIB_VAL
JZ LOAD_EIB_CK1000 LD_EIB_DIV LOAD_EIB_TENS
ITO LOAD_EIB_A1000   Add   El1=LD_EIB_DIV   El2=ASCII_0   Exit=LD_TMP
ITO LOAD_EIB_W1000   Write El1=LD_ET_OUT_PTR El2=LD_TMP
ITO LOAD_EIB_I1000   Add   El1=LD_ET_OUT_PTR El2=C_1           Exit=LD_ET_OUT_PTR
/Tens
ITO LOAD_EIB_TENS    Div   El1=LD_EIB_VAL   El2=C_10          Exit=LD_EIB_DIV
ITO LOAD_EIB_RTENS   Rem   El1=LD_EIB_VAL   El2=C_10          Exit=LD_EIB_VAL
JZ LOAD_EIB_CKTENS LD_EIB_DIV LOAD_EIB_ONES
ITO LOAD_EIB_ATENS   Add   El1=LD_EIB_DIV   El2=ASCII_0   Exit=LD_TMP
ITO LOAD_EIB_WTENS   Write El1=LD_ET_OUT_PTR El2=LD_TMP
ITO LOAD_EIB_ITENS   Add   El1=LD_ET_OUT_PTR El2=C_1           Exit=LD_ET_OUT_PTR
/Ones (always emit)
ITO LOAD_EIB_ONES    Add   El1=LD_EIB_VAL   El2=ASCII_0   Exit=LD_TMP
ITO LOAD_EIB_WONES   Write El1=LD_ET_OUT_PTR El2=LD_TMP
ITO LOAD_EIB_IONES   Add   El1=LD_ET_OUT_PTR El2=C_1           Exit=LD_ET_OUT_PTR
RREDI LOAD_EIB_RRET
/── LOAD_PROCESS_BODY: process body lines for SAVE ──────────
/Reads each line from LD_BODY_BUF and dispatches.
NEWREF LOAD_PROCESS_BODY LOAD_PB_IMPL
NEW LD_PB_PTR
NOLINK
ITO LOAD_PB_IMPL     Move  El1=LD_BODY_BUF_BASE_VAL Exit=LD_PB_PTR
ITO LOAD_PB_LOOP     Equal El1=LD_PB_PTR    El2=LD_BODY_PTR   Exit=LD_FLAG
ITO LOAD_PB_LCKJ     JumpIf El1=LD_FLAG     Exit=LOAD_PB_DONE
ITO LOAD_PB_RBYTE    Read  El1=LD_PB_PTR    Exit=LD_TMP
/Skip indent marker bytes (C_1, C_2) written by LOAD_READ_BODY
JEQ LOAD_PB_MRK1     LD_TMP C_1 LOAD_PB_SKIP
JEQ LOAD_PB_MRK2     LD_TMP C_2 LOAD_PB_SKIP
ITO LOAD_PB_LFCK     Equal El1=LD_TMP       El2=LF         Exit=LD_FLAG
ITO LOAD_PB_LFJ      JumpIf El1=LD_FLAG     Exit=LOAD_PB_DISPATCH
ITO LOAD_PB_INC      Add   El1=LD_PB_PTR    El2=C_1           Exit=LD_PB_PTR
ITO LOAD_PB_JMP      Jump  Exit=LOAD_PB_LOOP
NOLINK
ITO LOAD_PB_SKIP     Add   El1=LD_PB_PTR    El2=C_1           Exit=LD_PB_PTR
ITO LOAD_PB_SKIP_JMP Jump  Exit=LOAD_PB_LOOP
NOLINK
RVOCA LOAD_PB_DISPATCH   LOAD_DISPATCH_LINE
ITO LOAD_PB_INC2     Add   El1=LD_PB_PTR    El2=C_1           Exit=LD_PB_PTR
ITO LOAD_PB_JMP2     Jump  Exit=LOAD_PB_LOOP
NOLINK
RREDI LOAD_PB_DONE
/── SAVE_EMIT_SAVES: alloc Move reg→S_reg ITO luces ───────────
/MA1..MA7 = register lux addrs. For each non-zero:
/  Alloc 1-lux scratch S_reg, store addr in SAVE_ES_SREG_1..7
/  Alloc ITO: Move El1=reg Exit=S_reg
NEWREF SAVE_EMIT_SAVES SAVE_ES_IMPL
NEW SAVE_ES_SREG_1  /S_reg addr for MA1
NEW SAVE_ES_SREG_2  /S_reg addr for MA2
NEW SAVE_ES_SREG_3  /S_reg addr for MA3
NEW SAVE_ES_SREG_4  /S_reg addr for MA4
NEW SAVE_ES_SREG_5  /S_reg addr for MA5
NEW SAVE_ES_SREG_6  /S_reg addr for MA6
NEW SAVE_ES_SREG_7  /S_reg addr for MA7
NEW SAVE_ES_SREG    /current S_reg addr (used by SAVE_EMIT_ONE_SAVE)
NEW SAVE_ES_SDST    /current SAVE_ES_SREG_N lux to write into
NOLINK
JZ SAVE_ES_IMPL RA_MA1 SAVE_ES_DONE
ITO SAVE_ES_D1       Move  El1=SAVE_ES_SREG_1 Exit=SAVE_ES_SDST
RVOCA SAVE_ES_DO1    SAVE_EMIT_ONE_SAVE
JZ SAVE_ES_CK2 RA_MA2 SAVE_ES_DONE
ITO SAVE_ES_MV2      Move  El1=RA_MA2       Exit=RA_MA1
ITO SAVE_ES_D2       Move  El1=SAVE_ES_SREG_2 Exit=SAVE_ES_SDST
RVOCA SAVE_ES_DO2    SAVE_EMIT_ONE_SAVE
JZ SAVE_ES_CK3 RA_MA3 SAVE_ES_DONE
ITO SAVE_ES_MV3      Move  El1=RA_MA3       Exit=RA_MA1
ITO SAVE_ES_D3       Move  El1=SAVE_ES_SREG_3 Exit=SAVE_ES_SDST
RVOCA SAVE_ES_DO3    SAVE_EMIT_ONE_SAVE
JZ SAVE_ES_CK4 RA_MA4 SAVE_ES_DONE
ITO SAVE_ES_MV4      Move  El1=RA_MA4       Exit=RA_MA1
ITO SAVE_ES_D4       Move  El1=SAVE_ES_SREG_4 Exit=SAVE_ES_SDST
RVOCA SAVE_ES_DO4    SAVE_EMIT_ONE_SAVE
RREDI SAVE_ES_DONE
/── SAVE_EMIT_ONE_SAVE: alloc S_reg lux + Move lux ─────────
/MA1 = register addr. SAVE_ES_SDST = where to store S_reg addr.
/Allocs: 1 scratch lux (S_reg), 1 ITO Move El1=reg Exit=S_reg.
NEWREF SAVE_EMIT_ONE_SAVE SAVE_EOS_IMPL
NOLINK
/Alloc S_reg: 1 lux
ALLOC_TO SAVE_EOS_IMPL SAVE_ES_SREG C_1
/Write S_reg addr into the per-reg slot (SAVE_ES_SDST)
ITO SAVE_EOS_SDST    Write El1=SAVE_ES_SDST El2=SAVE_ES_SREG
/Alloc ITO Move El1=reg Exit=S_reg
ITO SAVE_EOS_ICN     Move  El1=ITO_SIZE     Exit=RA_ALLOC_COUNT
RVOCA SAVE_EOS_IAC   ALLOC_LUCES
ITO SAVE_EOS_ISELF   Write El1=RA_ALLOC_RESULT El2=RA_ALLOC_RESULT
ITO SAVE_EOS_IOP     Add   El1=RA_ALLOC_RESULT El2=C_1         Exit=LD_TMP
ITO SAVE_EOS_IOPW    Write El1=LD_TMP       El2=Move
ITO SAVE_EOS_IE1     Add   El1=RA_ALLOC_RESULT El2=C_2         Exit=LD_TMP
ITO SAVE_EOS_IE1W    Write El1=LD_TMP       El2=RA_MA1
ITO SAVE_EOS_IEX     Add   El1=RA_ALLOC_RESULT El2=C_4         Exit=LD_TMP
ITO SAVE_EOS_IEXW    Write El1=LD_TMP       El2=SAVE_ES_SREG
/Autolink
JZ SAVE_EOS_LCKZ RA_MC_PREV SAVE_EOS_NOAUTO
ITO SAVE_EOS_LADD    Add   El1=RA_MC_PREV   El2=C_5           Exit=LD_TMP
ITO SAVE_EOS_LW      Write El1=LD_TMP       El2=RA_ALLOC_RESULT
ITO SAVE_EOS_NOAUTO  Move  El1=RA_ALLOC_RESULT Exit=RA_MC_PREV
RREDI SAVE_EOS_RRET
/── SAVE_EMIT_RESTORES: emit Move S_reg→reg luces ─────────────
/Mirror of SAVE_EMIT_SAVES but reversed: reads SAVE_ES_SREG_N for S_reg addr.
NEWREF SAVE_EMIT_RESTORES SAVE_ER_IMPL
NOLINK
JZ SAVE_ER_IMPL RA_MA1 SAVE_ER_DONE
ITO SAVE_ER_SD1      Read  El1=SAVE_ES_SREG_1 Exit=SAVE_ES_SREG
RVOCA SAVE_ER_DO1    SAVE_EMIT_ONE_RESTORE
JZ SAVE_ER_CK2 RA_MA2 SAVE_ER_DONE
ITO SAVE_ER_MV2      Move  El1=RA_MA2       Exit=RA_MA1
ITO SAVE_ER_SD2      Read  El1=SAVE_ES_SREG_2 Exit=SAVE_ES_SREG
RVOCA SAVE_ER_DO2    SAVE_EMIT_ONE_RESTORE
JZ SAVE_ER_CK3 RA_MA3 SAVE_ER_DONE
ITO SAVE_ER_MV3      Move  El1=RA_MA3       Exit=RA_MA1
ITO SAVE_ER_SD3      Read  El1=SAVE_ES_SREG_3 Exit=SAVE_ES_SREG
RVOCA SAVE_ER_DO3    SAVE_EMIT_ONE_RESTORE
RREDI SAVE_ER_DONE
/── SAVE_EMIT_ONE_RESTORE: alloc Move S_reg→reg ITO ──────────
/MA1 = reg addr, SAVE_ES_SREG = S_reg addr (read from SAVE_ES_SREG_N by caller).
NEWREF SAVE_EMIT_ONE_RESTORE SAVE_EOR_IMPL
NOLINK
ITO SAVE_EOR_IMPL     Move  El1=ITO_SIZE     Exit=RA_ALLOC_COUNT
RVOCA SAVE_EOR_IAC   ALLOC_LUCES
ITO SAVE_EOR_ISELF   Write El1=RA_ALLOC_RESULT El2=RA_ALLOC_RESULT
ITO SAVE_EOR_IOP     Add   El1=RA_ALLOC_RESULT El2=C_1         Exit=LD_TMP
ITO SAVE_EOR_IOPW    Write El1=LD_TMP       El2=Move
ITO SAVE_EOR_IE1     Add   El1=RA_ALLOC_RESULT El2=C_2         Exit=LD_TMP
ITO SAVE_EOR_IE1W    Write El1=LD_TMP       El2=SAVE_ES_SREG
ITO SAVE_EOR_IEX     Add   El1=RA_ALLOC_RESULT El2=C_4         Exit=LD_TMP
ITO SAVE_EOR_IEXW    Write El1=LD_TMP       El2=RA_MA1
JZ SAVE_EOR_LCKZ RA_MC_PREV SAVE_EOR_NOAUTO
ITO SAVE_EOR_LADD    Add   El1=RA_MC_PREV   El2=C_5           Exit=LD_TMP
ITO SAVE_EOR_LW      Write El1=LD_TMP       El2=RA_ALLOC_RESULT
ITO SAVE_EOR_NOAUTO  Move  El1=RA_ALLOC_RESULT Exit=RA_MC_PREV
RREDI SAVE_EOR_RRET
/── LOAD_APPLY_BACKFILL: resolve pending forward references ────
/Walk LD_BACKFILL_BUF pairs: (slot_addr, name_hash).
/For each: HT_LOOKUP name_hash → if found → write addr into slot.
/Pairs end when slot_addr == 0.
NEW LD_ABF_PTR    /current read position in backfill buf
NEW LD_ABF_SLOT   /slot addr to fill
NEW LD_ABF_HASH   /name hash to resolve
NOLINK
ITO LOAD_APPLY_BACKFILL    Move  El1=LD_BACKFILL_BASE Exit=LD_ABF_PTR
ITO LOAD_ABF_LOOP    Read  El1=LD_ABF_PTR   Exit=LD_ABF_SLOT
/slot==0 → end of list
JZ LOAD_ABF_CK0 LD_ABF_SLOT LOAD_ABF_DONE
/Read hash
ITO LOAD_ABF_HADD    Add   El1=LD_ABF_PTR   El2=C_1           Exit=LD_TMP
ITO LOAD_ABF_HREAD   Read  El1=LD_TMP       Exit=LD_ABF_HASH
/Lookup in htable
ITO LOAD_ABF_LHSH    Move  El1=LD_ABF_HASH  Exit=RA_HT_HASH
ITO LOAD_ABF_LBASE   Move  El1=BS_HT_BASE   Exit=RA_HT_BASE
ITO LOAD_ABF_LMASK   Move  El1=BS_HT_MASK   Exit=RA_HT_MASK
ITO LOAD_ABF_LSIZE   Move  El1=BS_HT_SIZE   Exit=RA_HT_SIZE
RVOCA LOAD_ABF_LK    HT_LOOKUP
/If found (RA_HT_RESULT != 0): write resolved addr into slot
JZ LOAD_ABF_RCKZ RA_HT_RESULT LOAD_ABF_NEXT
ITO LOAD_ABF_WRITE   Write El1=LD_ABF_SLOT  El2=RA_HT_RESULT
/Advance ptr by 2
ITO LOAD_ABF_NEXT    Add   El1=LD_ABF_PTR   El2=C_2           Exit=LD_ABF_PTR
ITO LOAD_ABF_JMP     Jump  Exit=LOAD_ABF_LOOP
NOLINK
RREDI LOAD_ABF_DONE
/── LOAD_INTERN: intern token with backfill support ───────────
/Unlike BS_INTERN (eager alloc), LOAD_INTERN:
/  - If found in htable: return addr in RA_BS_RESULT
/  - If NOT found: write (slot_addr=0, hash) to backfill list, return 0
/  Caller checks RA_BS_RESULT: if 0, the current write slot needs backfill.
/
/Protocol: caller sets LD_INTERN_SLOT before calling LOAD_INTERN.
/  LD_INTERN_SLOT = addr of the slot that needs to be filled.
/  If intern finds name: writes directly. If not: records in backfill.
/
/Use BS_INTERN for names that MUST exist (commands, aspects).
/Use LOAD_INTERN for operand names that may be forward-referenced.
NEW LD_INTERN_SLOT    /slot addr waiting for this name's addr
NOLINK
/Lookup in htable first
RVOCA LOAD_INTERN    BS_LOOKUP
JZ LOAD_INT_CK RA_BS_RESULT LOAD_INT_MISS
/Found: return directly
RREDI LOAD_INT_RRET
/Miss: record (LD_INTERN_SLOT, hash) in backfill list
CHAIN
    LOAD_INT_MISS   Write El1=LD_BACKFILL_PTR El2=LD_INTERN_SLOT
    LOAD_INT_BFINC  Add   El1=LD_BACKFILL_PTR El2=C_1         Exit=LD_BACKFILL_PTR
    LOAD_INT_BFHSH  Write El1=LD_BACKFILL_PTR El2=RA_LOAD_HASH
    LOAD_INT_BFINC2 Add   El1=LD_BACKFILL_PTR El2=C_1         Exit=LD_BACKFILL_PTR
    LOAD_INT_BFEND  Write El1=LD_BACKFILL_PTR El2=C_0
        CLEAR LOAD_INT_ZERO RA_BS_RESULT
        RREDI LOAD_INT_MRRET
