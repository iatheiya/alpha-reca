/lexer.re — File reader and tokenizer for the Reca loader
/
/Provides byte-level file I/O and token extraction over .re source files.
/All functions work through shared buffers (token buffer + read buffer)
/and shared registers from registers.re (RA_LOAD_BYTE, RA_LOAD_RPOS etc.).
/
/Functions:
/  BS_INIT         — initialize base-pointer registers from BLOCK self-refs
/  BS_READ_BYTE    — read next byte from file into RA_LOAD_BYTE (0 on EOF)
/  BS_SKIP_TO_EOL  — consume bytes until LF or EOF
/  BS_BLOCK_SKIP   — consume bytes until closing double-slash or EOF
/  BS_READ_TOKEN   — read next whitespace-delimited token into BS_TOKBUF
/  BS_TOKEN_VALUE  — strip "key=" prefix from current tokbuf, recompute hash
/  BS_SCAN_EQ      — scan tokbuf for '=', set RA_BS_FLAG=1 if found
/  BS_PACK_TOKBUF  — pack current token bytes into dense packed string
/  BS_PARSE_INT    — parse integer from tokbuf → RA_BS_TMP2
/
/DEPENDS ON: aspects.re constants.re ascii.re registers.re linux_generic.re
/            alloc.re (BS_PACK_TOKBUF uses ALLOC_LUCES)
/            intern.re (BS_HT_MASK for hash masking, RA_BS_TMP/FLAG/RESULT)

/── Token buffer ──────────────────────────────────────────────
/Holds the current token bytes, 1 byte per lux, 0-terminated.
/RA_LOAD_TLEN = number of valid bytes. RA_LOAD_HASH = djb2 hash.
NEWSET BS_TOKBUF_SIZE 256
BLOCK  BS_TOKBUF_000  256
SETREF BS_TOKBUF_000 BS_TOKBUF_000  /self-ref: BS_INIT reads word → base addr
NEW    BS_TOKBUF_BASE  /base lux addr (written by BS_INIT)
/RA_LOAD_TLEN, RA_LOAD_BYTE, RA_LOAD_HASH — declared in registers.re

/── Read buffer ───────────────────────────────────────────────
/Page-sized buffer for file I/O. Refilled by BS_READ_BYTE via SYS_READ.
NEWSET BS_READBUF_SIZE 4096
BLOCK  BS_READBUF_000  4096
SETREF BS_READBUF_000 BS_READBUF_000
NEW    BS_READBUF_BASE
/RA_LOAD_RPOS, RA_LOAD_RLEN, RA_LOAD_FD — declared in registers.re

/── Pack-tokbuf scratch ───────────────────────────────────────
NEW RA_BS_PACK_WORD   /accumulator: current u64 word being built
NEW RA_BS_PACK_SHIFT  /bit shift for next byte (0, 8, 16 ... 56)
NEW RA_BS_PACK_SIDX   /source index into tokbuf (0..tlen-1)
NEW RA_BS_PACK_DST    /destination lux pointer in aether

/── Shared scratch (also used by intern.re and saku.re) ───────
NEW RA_BS_TMP
NEW RA_BS_TMP2
NEW RA_BS_TMP3
NEW RA_BS_FLAG
/Dedicated result register for BS_PARSE_INT — isolated from RA_BS_TMP2 which
/is reused as scratch by BS_READ_TOKEN / BS_TOKEN_VALUE hash loops.
NEW RA_BS_PIVAL
SETREF RA_BS_FLAG RA_BS_FLAG  /self-ref: Equal writes here, JumpIf reads here

/── djb2 initial value ────────────────────────────────────────
NEWSET BS_HASH0   5381

/── BS_INIT: initialize base-pointer registers ────────────────
/Must be called once before BS_READ_BYTE or BS_READ_TOKEN.
/Reads self-ref words from BLOCK headers to get lux addresses.
CHAIN BS_INIT
    Move  El1=BS_HTAB_000    Exit=BS_HT_BASE
    Move  El1=BS_TOKBUF_000  Exit=BS_TOKBUF_BASE
    Move  El1=BS_READBUF_000 Exit=BS_READBUF_BASE
RREDI BS_INIT_RET

/── BS_READ_BYTE: read next byte → RA_LOAD_BYTE (0 on EOF) ───
/Refills read buffer via SYS_READ when RA_LOAD_RPOS == RA_LOAD_RLEN.
NOLINK
JZ BS_READ_BYTE RA_REDIRECT_BASE BS_RB_LIVE_CHECK
/Redirect mode active: read from the already-constructed buffer instead
/of the live file stream. RA_LOAD_RPOS/RA_LOAD_RLEN are left untouched.
JEQ BS_RB_RDEOF RA_REDIRECT_POS RA_REDIRECT_LEN BS_RB_RDDONE
ITO BS_RB_RDADDR  Add   El1=RA_REDIRECT_BASE El2=RA_REDIRECT_POS Exit=RA_BS_TMP
ITO BS_RB_RDLOAD  Read  El1=RA_BS_TMP        Exit=RA_LOAD_BYTE
ITO BS_RB_RDINC   Add   El1=RA_REDIRECT_POS  El2=C_1             Exit=RA_REDIRECT_POS
RREDI BS_RB_RDRET
NOLINK
CLEAR BS_RB_RDDONE RA_LOAD_BYTE
RREDI BS_RB_RDDRET
NOLINK
JEQ BS_RB_LIVE_CHECK RA_LOAD_RPOS RA_LOAD_RLEN BS_RB_FILL
ITO BS_RB_BADDR   Add   El1=BS_READBUF_BASE El2=RA_LOAD_RPOS  Exit=RA_BS_TMP
ITO BS_RB_LOAD    Read  El1=RA_BS_TMP       Exit=RA_LOAD_BYTE
ITO BS_RB_INC     Add   El1=RA_LOAD_RPOS    El2=C_1           Exit=RA_LOAD_RPOS
RREDI BS_RB_RET
CHAIN
    BS_RB_FILL    Move  El1=SYS_READ        Exit=SC_NR
    BS_RB_FD      Move  El1=RA_LOAD_FD      Exit=SC_A0
    BS_RB_BUF     Move  El1=BS_READBUF_BASE Exit=SC_A1
    BS_RB_SZ      Move  El1=BS_READBUF_SIZE Exit=SC_A2
    BS_RB_SYSCALL Exire El1=C_0             El2=C_0   Exit=C_0
    BS_RB_RLEN    Move  El1=SC_A0           Exit=RA_LOAD_RLEN
NOLINK
CLEAR BS_RB_POS RA_LOAD_RPOS
JZ BS_RB_EOFCK RA_LOAD_RLEN BS_RB_EOF
ITO BS_RB_RETRY Jump  Exit=BS_READ_BYTE
NOLINK
CLEAR BS_RB_EOF RA_LOAD_BYTE
RREDI BS_RB_EOFRET

/── BS_SKIP_TO_EOL: consume bytes until LF or EOF ─────────────
NOLINK
NEWREF BS_SKIP_TO_EOL BS_SKIP_EOL  /alias: both names point here
ITO BS_SKIP_EOL   Jump  Exit=BS_SE_GUARD
NOLINK
/Guard: if the caller's last token-read already stopped exactly at LF/EOF
/(e.g. a token immediately followed by newline, no trailing inline
/comment), scanning forward here would search for the *next* LF — i.e.
/it would silently consume the ENTIRE FOLLOWING LINE, since this routine
/has no notion of "already at a line boundary", only "find the next LF".
JZ BS_SE_GUARD RA_LOAD_BYTE BS_SE_DONE
JEQ BS_SE_GLFCK RA_LOAD_BYTE LF BS_SE_DONE
ITO BS_SE_GJMP    Jump  Exit=BS_SE_RD
NOLINK
RVOCA BS_SE_RD    BS_READ_BYTE
JZ BS_SE_EOF RA_LOAD_BYTE BS_SE_DONE
JEQ BS_SE_LF RA_LOAD_BYTE LF BS_SE_DONE
ITO BS_SE_LOOP    Jump  Exit=BS_SE_RD
NOLINK
RREDI BS_SE_DONE

/── BS_BLOCK_SKIP: skip until closing double-slash or EOF ─────
/Entry: file positioned just after opening double-slash.
/Handles multi-line blocks.
NOLINK
RVOCA BS_BLOCK_SKIP   BS_READ_BYTE
JZ BS_BLK_EOF RA_LOAD_BYTE BS_BLK_DONE
JEQ BS_BLK_SL RA_LOAD_BYTE SLASH BS_BLK_SL2B
ITO BS_BLK_CONT   Jump  Exit=BS_BLOCK_SKIP
NOLINK
RVOCA BS_BLK_SL2B BS_READ_BYTE
JEQ BS_BLK_SL2CK RA_LOAD_BYTE SLASH BS_BLK_DONE
ITO BS_BLK_LONE   Jump  Exit=BS_BLOCK_SKIP
NOLINK
RREDI BS_BLK_DONE

/── BS_READ_TOKEN: read next whitespace-delimited token ────────
/Skips leading whitespace (SP, TAB, CR). Stops at LF or EOF.
/Single '/' → single-line comment: skip to EOL, return empty token.
/Double '//' → inline block: skip block, continue scanning for token.
/OUT: BS_TOKBUF_BASE[0..RA_LOAD_TLEN-1] = token bytes
/     RA_LOAD_HASH = djb2 hash masked to BS_HT_MASK
/     RA_LOAD_BYTE = 0 (EOF), LF (end of line), or terminator byte
NOLINK
CLEAR BS_READ_TOKEN RA_LOAD_TLEN
ITO BS_RT_HASH0   Move  El1=BS_HASH0     Exit=RA_LOAD_HASH
ITO BS_RT_JMP2SK  Jump  Exit=BS_RT_SKP
NOLINK
ITO BS_RT_SKP     Jump  Exit=BS_RT_SKP_RD
NOLINK
ITO BS_RT_SKP_RD  Jump  Exit=BS_RT_SKP_RD2
NOLINK
RVOCA BS_RT_SKP_RD2  BS_READ_BYTE
JZ BS_RT_EOF RA_LOAD_BYTE BS_RT_DONE
JEQ BS_RT_LF RA_LOAD_BYTE LF BS_RT_DONE
JEQ BS_RT_SL RA_LOAD_BYTE SLASH BS_RT_SL2B
SWITCH RA_LOAD_BYTE
    SP    BS_RT_SKP_RD2
    TAB   BS_RT_SKP_RD2
    CR    BS_RT_SKP_RD2
ITO BS_RT_COL     Jump  Exit=BS_RT_COLLECT
NOLINK
RVOCA BS_RT_SL2B  BS_READ_BYTE
JEQ BS_RT_SL2CK RA_LOAD_BYTE SLASH BS_RT_BLK
RVOCA BS_RT_CSKP  BS_SKIP_TO_EOL
ITO BS_RT_CDONE   Jump  Exit=BS_RT_DONE
NOLINK
RVOCA BS_RT_BLK   BS_BLOCK_SKIP
ITO BS_RT_BLKDNE  Jump  Exit=BS_RT_SKP
NOLINK
ITO BS_RT_COLLECT Jump  Exit=BS_RT_COL2
NOLINK
ITO BS_RT_COL2    Add   El1=BS_TOKBUF_BASE El2=RA_LOAD_TLEN  Exit=RA_BS_TMP
ITO BS_RT_STORE   Write El1=RA_BS_TMP      El2=RA_LOAD_BYTE
ITO BS_RT_H33     Mul   El1=RA_LOAD_HASH   El2=C_33          Exit=RA_BS_TMP2
ITO BS_RT_HADD    Add   El1=RA_BS_TMP2     El2=RA_LOAD_BYTE  Exit=RA_LOAD_HASH
ITO BS_RT_INC     Add   El1=RA_LOAD_TLEN   El2=C_1           Exit=RA_LOAD_TLEN
RVOCA BS_RT_NB    BS_READ_BYTE
SWITCH RA_LOAD_BYTE
    0     BS_RT_DONE
    SP    BS_RT_DONE
    TAB   BS_RT_DONE
    LF    BS_RT_DONE
    CR    BS_RT_DONE
    SLASH BS_RT_SLRB
ITO BS_RT_MORE    Jump  Exit=BS_RT_COL2
NOLINK
RVOCA BS_RT_SLRB  BS_READ_BYTE
JEQ BS_RT_SLCK2 RA_LOAD_BYTE SLASH BS_RT_SLBK
ITO BS_RT_SLEND   Jump  Exit=BS_RT_DONE
NOLINK
RVOCA BS_RT_SLBK  BS_BLOCK_SKIP
ITO BS_RT_SLBND   Jump  Exit=BS_RT_DONE
NOLINK
RREDI BS_RT_DONE

/── BS_TOKEN_VALUE: strip "key=" prefix, recompute hash ───────
/If tokbuf contains '=': copy bytes after '=' to front, update RA_LOAD_TLEN,
/recompute RA_LOAD_HASH for the value portion.
/If no '=': return as-is (whole token is the value).
NOLINK
CLEAR BS_TOKEN_VALUE RA_BS_TMP3
ITO BS_TV_IDX_JMP2BS_TV_LO  Jump  Exit=BS_TV_LOOP
NOLINK
JEQ BS_TV_LOOP RA_BS_TMP3 RA_LOAD_TLEN BS_TV_NOEQ
ITO BS_TV_BADDR   Add   El1=BS_TOKBUF_BASE El2=RA_BS_TMP3  Exit=RA_BS_TMP
ITO BS_TV_BYTE    Read  El1=RA_BS_TMP      Exit=RA_BS_TMP
JEQ BS_TV_EQC RA_BS_TMP EQUALS BS_TV_FOUND
ITO BS_TV_INC     Add   El1=RA_BS_TMP3     El2=C_1         Exit=RA_BS_TMP3
ITO BS_TV_JMP     Jump  Exit=BS_TV_LOOP
NOLINK
RREDI BS_TV_NOEQ
NOLINK
ITO BS_TV_FOUND   Add   El1=RA_BS_TMP3     El2=C_1         Exit=RA_BS_TMP3
CLEAR BS_TV_VIDX RA_BS_TMP2
ITO BS_TV_VIDX_JMP2BS_TV_CL  Jump  Exit=BS_TV_CLOOP
NOLINK
JEQ BS_TV_CLOOP RA_BS_TMP3 RA_LOAD_TLEN BS_TV_CDONE
CHAIN BS_TV_CSRC
    Add   El1=BS_TOKBUF_BASE El2=RA_BS_TMP3  Exit=RA_BS_TMP
    Read  El1=RA_BS_TMP      Exit=RA_BS_TMP
    Add   El1=BS_TOKBUF_BASE El2=RA_BS_TMP2  Exit=RA_BS_FLAG
    Write El1=RA_BS_FLAG     El2=RA_BS_TMP
    Add   El1=RA_BS_TMP3     El2=C_1         Exit=RA_BS_TMP3
    Add   El1=RA_BS_TMP2     El2=C_1         Exit=RA_BS_TMP2
    Jump  Exit=BS_TV_CLOOP
NOLINK
ITO BS_TV_CDONE   Move  El1=RA_BS_TMP2     Exit=RA_LOAD_TLEN
CLEAR BS_TV_HIDX RA_BS_TMP2
ITO BS_TV_HASH0   Move  El1=BS_HASH0       Exit=RA_LOAD_HASH
ITO BS_TV_HASH0_JMP2BS_TV_HL  Jump  Exit=BS_TV_HLOOP
NOLINK
JEQ BS_TV_HLOOP RA_BS_TMP2 RA_LOAD_TLEN BS_TV_HDONE
CHAIN BS_TV_HBADDR
    Add   El1=BS_TOKBUF_BASE El2=RA_BS_TMP2  Exit=RA_BS_TMP
    Read  El1=RA_BS_TMP      Exit=RA_BS_TMP
    Mul   El1=RA_LOAD_HASH   El2=C_33        Exit=RA_BS_TMP3
    Add   El1=RA_BS_TMP3     El2=RA_BS_TMP   Exit=RA_LOAD_HASH
    Add   El1=RA_BS_TMP2     El2=C_1         Exit=RA_BS_TMP2
    Jump  Exit=BS_TV_HLOOP
NOLINK
RREDI BS_TV_HDONE

/── BS_SCAN_EQ: scan tokbuf for '=' → RA_BS_FLAG ─────────────
/RA_BS_FLAG = 1 if '=' found, 0 otherwise. Does not modify tokbuf.
NOLINK
CLEAR BS_SCAN_EQ RA_BS_TMP3
ITO BS_SE2_START  Jump  Exit=BS_SE2_LOOP
NOLINK
JEQ BS_SE2_LOOP RA_BS_TMP3 RA_LOAD_TLEN BS_SE2_NF
ITO BS_SE2_ADDR   Add   El1=BS_TOKBUF_BASE El2=RA_BS_TMP3  Exit=RA_BS_TMP
ITO BS_SE2_BYTE   Read  El1=RA_BS_TMP      Exit=RA_BS_TMP
JEQ BS_SE2_EQC RA_BS_TMP EQUALS BS_SE2_FOUND
ITO BS_SE2_INC    Add   El1=RA_BS_TMP3     El2=C_1         Exit=RA_BS_TMP3
ITO BS_SE2_JMP    Jump  Exit=BS_SE2_LOOP
NOLINK
ITO BS_SE2_FOUND  Move  El1=C_1            Exit=RA_BS_FLAG
RREDI BS_SE2_FRRET
NOLINK
CLEAR BS_SE2_NF RA_BS_FLAG
RREDI BS_SE2_NRRET

/── BS_PACK_TOKBUF: pack token bytes into dense string ─────────
/IN:  BS_TOKBUF_BASE (1 byte/lux), RA_LOAD_TLEN = byte count
/OUT: RA_BS_TMP2 = addr of first packed lux (stride=1, NUL-terminated)
/Clobbers: RA_BS_TMP, RA_BS_TMP3, RA_BS_PACK_WORD/SHIFT/SIDX/DST
NOLINK
ITO BS_PACK_TOKBUF  Add   El1=RA_LOAD_TLEN  El2=C_7   Exit=RA_BS_TMP
ITO BPT_LUCES2      Div   El1=RA_BS_TMP     El2=C_8   Exit=RA_ALLOC_COUNT
ITO BPT_LUCES3      Add   El1=RA_ALLOC_COUNT El2=C_2  Exit=RA_ALLOC_COUNT
RVOCA BPT_ALLOC     ALLOC_LUCES
ITO BPT_SAVEDS      Move  El1=RA_ALLOC_RESULT Exit=RA_BS_TMP2
ITO BPT_INITDST     Move  El1=RA_ALLOC_RESULT Exit=RA_BS_PACK_DST
CLEAR BPT_IS RA_BS_PACK_SIDX
CLEAR BPT_IW RA_BS_PACK_WORD
CLEAR BPT_ISH RA_BS_PACK_SHIFT
JEQ BPT_LOOP RA_BS_PACK_SIDX RA_LOAD_TLEN BPT_FLUSH
ITO BPT_BADDR       Add   El1=BS_TOKBUF_BASE El2=RA_BS_PACK_SIDX Exit=RA_BS_TMP
ITO BPT_BLOAD       Read  El1=RA_BS_TMP     Exit=RA_BS_TMP3
ITO BPT_BSHIFT      Left  El1=RA_BS_TMP3    El2=RA_BS_PACK_SHIFT Exit=RA_BS_TMP3
ITO BPT_BOR         Add   El1=RA_BS_PACK_WORD El2=RA_BS_TMP3  Exit=RA_BS_PACK_WORD
ITO BPT_SIDINC      Add   El1=RA_BS_PACK_SIDX El2=C_1   Exit=RA_BS_PACK_SIDX
ITO BPT_SHINC       Add   El1=RA_BS_PACK_SHIFT El2=C_8  Exit=RA_BS_PACK_SHIFT
JEQ BPT_FULL RA_BS_PACK_SHIFT C_64 BPT_FLUSH_WORD
ITO BPT_LBJMP       Jump  Exit=BPT_LOOP
NOLINK
ITO BPT_FLUSH_WORD  Write El1=RA_BS_PACK_DST El2=RA_BS_PACK_WORD
ITO BPT_DSTINC      Add   El1=RA_BS_PACK_DST El2=C_1   Exit=RA_BS_PACK_DST
CLEAR BPT_WCLR RA_BS_PACK_WORD
CLEAR BPT_SCLR RA_BS_PACK_SHIFT
ITO BPT_FWJMP       Jump  Exit=BPT_LOOP
NOLINK
ITO BPT_FLUSH       Write El1=RA_BS_PACK_DST El2=RA_BS_PACK_WORD
ITO BPT_NULLINC     Add   El1=RA_BS_PACK_DST El2=C_1   Exit=RA_BS_PACK_DST
CLEAR BPT_NULL RA_BS_TMP3
ITO BPT_NULLW       Write El1=RA_BS_PACK_DST El2=RA_BS_TMP3
RREDI BPT_RRET

/── BS_PARSE_INT: parse integer from tokbuf → RA_BS_PIVAL ──────
/Handles optional '-' prefix. Stops at first non-digit byte.
/IN:  BS_TOKBUF_BASE[0..RA_LOAD_TLEN-1] = decimal digits
/OUT: RA_BS_PIVAL = signed integer value (dedicated reg, no scratch collision)
NOLINK
CLEAR BS_PARSE_INT RA_BS_PIVAL
CLEAR BS_PI_IDX RA_BS_TMP3
CLEAR BS_PI_NEG RA_BS_FLAG
CLEAR BS_PI_NEGF RA_BS_TMP2
ITO BS_PI_RB0     Read  El1=BS_TOKBUF_BASE Exit=RA_BS_TMP
JEQ BS_PI_DCHK RA_BS_TMP MINUS BS_PI_NEG_SET
ITO BS_PI_START   Jump  Exit=BS_PI_LOOP
NOLINK
ITO BS_PI_NEG_SET Move  El1=C_1           Exit=RA_BS_TMP2
ITO BS_PI_NIDX    Add   El1=RA_BS_TMP3    El2=C_1   Exit=RA_BS_TMP3
ITO BS_PI_LOOP2   Jump  Exit=BS_PI_LOOP
NOLINK
JEQ BS_PI_LOOP RA_BS_TMP3 RA_LOAD_TLEN BS_PI_DONE
ITO BS_PI_BADDR   Add   El1=BS_TOKBUF_BASE El2=RA_BS_TMP3 Exit=RA_BS_TMP
ITO BS_PI_BYTE    Read  El1=RA_BS_TMP     Exit=RA_BS_TMP
ITO BS_PI_DCK0    ULess El1=RA_BS_TMP     El2=ASCII_0    Exit=RA_BS_FLAG
ITO BS_PI_DCK0J   JumpIf El1=RA_BS_FLAG  Exit=BS_PI_DONE
ITO BS_PI_DCK9    ULess El1=ASCII_9       El2=RA_BS_TMP  Exit=RA_BS_FLAG
ITO BS_PI_DCK9J   JumpIf El1=RA_BS_FLAG  Exit=BS_PI_DONE
ITO BS_PI_SUB0    Sub   El1=RA_BS_TMP     El2=ASCII_0    Exit=RA_BS_TMP
ITO BS_PI_MUL     Mul   El1=RA_BS_PIVAL   El2=C_10       Exit=RA_BS_PIVAL
ITO BS_PI_ADD     Add   El1=RA_BS_PIVAL   El2=RA_BS_TMP  Exit=RA_BS_PIVAL
ITO BS_PI_INC     Add   El1=RA_BS_TMP3    El2=C_1        Exit=RA_BS_TMP3
ITO BS_PI_JMP     Jump  Exit=BS_PI_LOOP
NOLINK
ITO BS_PI_DONE    JumpIf El1=RA_BS_TMP2  Exit=BS_PI_NEGATE
RREDI BS_PI_OKRET
NOLINK
ITO BS_PI_NEGATE  Sub   El1=C_0           El2=RA_BS_PIVAL Exit=RA_BS_PIVAL
RREDI BS_PI_NRRET
