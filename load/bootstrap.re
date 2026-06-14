//============================================================
bootstrap.re — Single-pass Reca bootstrap loader

Replaces loader.py Wave-A/Wave-B. Python only:
  1. Writes 24 aspect self-refs into aether
  2. Writes file list at BS_FILE_LIST_BASE
  3. Sets BS_CURSOR to first free address after bootstrap data
  4. Calls execute(BS_MAIN)

BS_MAIN reads each file byte-by-byte, tokenizes, dispatches:
  NEW name       → bump-alloc 1 lux, register in BS_SYMTAB
  SET name val   → lookup name, write val (int or symbol addr)
  LINK s r t     → lookup all three, append lumen pair to src
  anything else  → lookup CMD in symbol table, call as Reca program
  unknown name   → lazy alloc: bump 1 lux, register (forward-ref)

One wave per file, two sweeps total:
  Wave A (all files): NEW only → all names exist
  Wave B (all files): everything else → wiring

DEPENDS ON: aspects.re constants.re ascii.re regs.re alloc.re htable.re
            linux_generic.re
//============================================================

/── Bootstrap-specific registers ──────────────────────────────
NEW BS_CURSOR        /bump allocator cursor (set by Python before BS_MAIN)
NEW BS_FILE_LIST     /addr of first file path packed-string (set by Python)
NEW BS_FILE_COUNT    /number of files to load (set by Python)
NEW BS_SWEEP         /0 = sweep A (NEW only), 1 = sweep B (wire)

/── Symbol table (hash table in aether) ───────────────────────
/HT slot: (hash32 << 32) | addr32. Size = 4096 slots = power of 2.
NEWSET BS_HT_SIZE  262144
NEWSET BS_HT_MASK  262143
BLOCK  BS_HTAB_000 262144  /symbol hash table storage (4× for self-hosting)
SETREF BS_HTAB_000 BS_HTAB_000  /self-ref so BS_INIT: Move El1=BS_HTAB_000 → addr

NEW BS_HT_BASE              /base addr of BS_HTAB (set at init)
NEW RA_BS_RESULT            /result of lookup (addr or 0)

/── Token buffer ───────────────────────────────────────────────
NEWSET BS_TOKBUF_SIZE 256
BLOCK  BS_TOKBUF_000  256   /token bytes, 1 byte per lux
SETREF BS_TOKBUF_000 BS_TOKBUF_000
NEW    BS_TOKBUF_BASE        /base addr (set at init)
/RA_LOAD_TLEN, RA_LOAD_BYTE — declared in runtime/registers.re

/── Read buffer (1 page per file read) ────────────────────────
NEWSET BS_READBUF_SIZE 4096
BLOCK  BS_READBUF_000  4096
SETREF BS_READBUF_000 BS_READBUF_000
NEW    BS_READBUF_BASE
/RA_LOAD_RPOS, RA_LOAD_RLEN, RA_LOAD_FD — declared in runtime/registers.re

/Call stack (for RCALL/RRET macros): REMOVED. BS_CS_BUF_000 (1024-entry
/array) and BS_CS_SP have been removed — RCALL/RRET were unused and have
/been deleted from macros.re. The automatic call stack (RA_SP, see
/runtime/regs.re) replaced this entirely.

/── Element registers (tokens after command) ─────────────────
NEW RA_BS_EL0    /first element addr (resolved)
NEW RA_BS_EL1    /second element addr
NEW RA_BS_EL2    /third element addr
NEW RA_BS_ELC    /element count parsed

/── Scratch ────────────────────────────────────────────────────
NEW RA_BS_TMP
NEW RA_BS_TMP2
NEW RA_BS_TMP3
NEW RA_BS_FLAG
SETREF RA_BS_FLAG RA_BS_FLAG  /self-ref: Equal writes here, JumpIf reads here
NEW RA_BS_FIDX    /file list index
NEW RA_BS_FPATH   /current file path packed-string addr
NEW RA_BS_PACK_WORD   /accumulates packed bytes (up to 8 per lux)
NEW RA_BS_PACK_SHIFT  /current bit shift within packed word (0,8,16,...,56)
NEW RA_BS_PACK_SIDX   /source index into tokbuf (0..tlen-1)
NEW RA_BS_PACK_DST    /destination lux pointer in aether

/── Constants needed locally ───────────────────────────────────
NEWSET BS_HASH0   5381  /djb2 initial value

/── BS_INIT: initialize base pointers ─────────────────────────
NOLINK
ITO BS_INIT          Move  El1=BS_HTAB_000    Exit=BS_HT_BASE
ITO BS_INIT2         Move  El1=BS_TOKBUF_000  Exit=BS_TOKBUF_BASE
ITO BS_INIT3         Move  El1=BS_READBUF_000 Exit=BS_READBUF_BASE
RREDI BS_INIT_RET

/── BS_MAIN: entry point ───────────────────────────────────────
NOLINK
CLEAR BS_MAIN BS_SWEEP
/Clear hash table
ITO BS_M_HTCL1   Move  El1=BS_HTAB_000  Exit=RA_HT_BASE
ITO BS_M_HTCL2   Move  El1=BS_HT_SIZE   Exit=RA_HT_SIZE
RVOCA BS_M_HTCLJ  HT_CLEAR
RVOCA BS_M_INIT   BS_INIT
/Wave A: load all NEWs
CLEAR BS_M_A_START RA_BS_FIDX
RVOCA BS_M_A_LOOP BS_LOAD_ALL_NEW
/Wave B: wire everything
ITO BS_M_B_SW    Move  El1=C_1    Exit=BS_SWEEP
CLEAR BS_M_B_START RA_BS_FIDX
RVOCA BS_M_B_LOOP BS_WIRE_ALL
/Done
RREDI BS_MAIN_RET

/── BS_LOAD_ALL_NEW: sweep A — iterate files, process NEW only ─
NOLINK
JEQ BS_LAN_CK RA_BS_FIDX BS_FILE_COUNT BS_LAN_DONE
/Load file path from file list
ITO BS_LAN_FADDR Add     El1=BS_FILE_LIST El2=RA_BS_FIDX    Exit=RA_BS_TMP
ITO BS_LAN_FLOAD Read    El1=RA_BS_TMP    Exit=RA_BS_FPATH
RVOCA BS_LAN_OPEN BS_OPEN_FILE
/Process file: NEW only
RVOCA BS_LAN_PROC BS_PROCESS_FILE_NEW
/Close fd
ITO BS_LAN_CLNR  Move    El1=SYS_CLOSE    Exit=SC_NR
ITO BS_LAN_CLFD  Move    El1=RA_LOAD_FD     Exit=SC_A0
ITO BS_LAN_CLXR  Exire   El1=C_0         El2=C_0            Exit=C_0
/Next file
ITO BS_LAN_INC   Add     El1=RA_BS_FIDX   El2=C_1           Exit=RA_BS_FIDX
ITO BS_LAN_JMP   Jump    Exit=BS_LAN_CK
RREDI BS_LAN_DONE

/── BS_WIRE_ALL: sweep B — iterate files, wire everything ──────
NOLINK
JEQ BS_WA_CK RA_BS_FIDX BS_FILE_COUNT BS_WA_DONE
ITO BS_WA_FADDR  Add     El1=BS_FILE_LIST El2=RA_BS_FIDX    Exit=RA_BS_TMP
ITO BS_WA_FLOAD  Read    El1=RA_BS_TMP    Exit=RA_BS_FPATH
RVOCA BS_WA_OPEN BS_OPEN_FILE
RVOCA BS_WA_PROC BS_PROCESS_FILE_WIRE
ITO BS_WA_CLNR   Move    El1=SYS_CLOSE    Exit=SC_NR
ITO BS_WA_CLFD   Move    El1=RA_LOAD_FD     Exit=SC_A0
ITO BS_WA_CLXR   Exire   El1=C_0         El2=C_0            Exit=C_0
ITO BS_WA_INC    Add     El1=RA_BS_FIDX   El2=C_1           Exit=RA_BS_FIDX
ITO BS_WA_JMP    Jump    Exit=BS_WA_CK
RREDI BS_WA_DONE

/── BS_OPEN_FILE: open RA_BS_FPATH → RA_LOAD_FD ─────────────────
/Uses syscall openat(AT_FDCWD=-100, path, O_RDONLY=0)
NEWSET BS_AT_FDCWD  -100
NEWSET BS_O_RDONLY  0
CHAIN
    BS_OPEN_FILE  Move   El1=SYS_OPENAT   Exit=SC_NR
    BS_OF_DIR     Move   El1=BS_AT_FDCWD  Exit=SC_A0
    BS_OF_PATH    Move   El1=RA_BS_FPATH  Exit=SC_A1
    BS_OF_FLAGS   Move   El1=BS_O_RDONLY  Exit=SC_A2
    BS_OF_EXIRE   Exire  El1=C_0         El2=C_0            Exit=C_0
    BS_OF_FD      Move   El1=SC_A0        Exit=RA_LOAD_FD
        CLEAR BS_OF_RPOS RA_LOAD_RPOS
        CLEAR BS_OF_RLEN RA_LOAD_RLEN
        RREDI BS_OF_RET

/── BS_READ_BYTE: read next byte → RA_LOAD_BYTE (0 on EOF) ──────
/Refills buffer via SYS_READ when needed.
NOLINK
JEQ BS_READ_BYTE RA_LOAD_RPOS RA_LOAD_RLEN BS_RB_FILL
/Return byte from buffer
ITO BS_RB_BADDR  Add     El1=BS_READBUF_BASE El2=RA_LOAD_RPOS Exit=RA_BS_TMP
ITO BS_RB_LOAD   Read    El1=RA_BS_TMP   Exit=RA_LOAD_BYTE
ITO BS_RB_INC    Add     El1=RA_LOAD_RPOS  El2=C_1           Exit=RA_LOAD_RPOS
RREDI BS_RB_RET
/Fill buffer
CHAIN _
        ITO BS_RB_FILL Move El1=SYS_READ Exit=SC_NR
    Move    El1=RA_LOAD_FD     Exit=SC_A0
    Move    El1=BS_READBUF_BASE Exit=SC_A1
    Move    El1=BS_READBUF_SIZE Exit=SC_A2
    Exire   El1=C_0         El2=C_0            Exit=C_0
    Move    El1=SC_A0        Exit=RA_LOAD_RLEN
        CLEAR BS_RB_POS RA_LOAD_RPOS
/If 0 bytes → EOF: return 0
        JZ BS_RB_EOFCK RA_LOAD_RLEN BS_RB_EOF
        ITO BS_RB_RETRY  Jump    Exit=BS_READ_BYTE
NOLINK
CLEAR BS_RB_EOF RA_LOAD_BYTE
RREDI BS_RB_EOFRET

/── BS_SKIP_TO_EOL: skip bytes until LF or EOF ─────────────────
NOLINK
NEWREF BS_SKIP_TO_EOL BS_SKIP_EOL  /alias
ITO BS_SKIP_EOL Jump    Exit=BS_SE_RD
NOLINK
RVOCA BS_SE_RD BS_READ_BYTE
JZ BS_SE_EOF RA_LOAD_BYTE BS_SE_DONE
JEQ BS_SE_LF RA_LOAD_BYTE LF BS_SE_DONE
ITO BS_SE_LOOP   Jump    Exit=BS_SE_RD
NOLINK
RREDI BS_SE_DONE
/── BS_READ_TOKEN: read next whitespace-delimited token ────────
/Skips leading whitespace. Returns token in BS_TOKBUF, length in RA_LOAD_TLEN.
/Hash in RA_LOAD_HASH. Sets RA_LOAD_BYTE=0 on EOF, RA_LOAD_TLEN=0 if line ended.
NOLINK
CLEAR BS_READ_TOKEN RA_LOAD_TLEN
ITO BS_RT_HASH0  Move   El1=BS_HASH0     Exit=RA_LOAD_HASH
ITO BS_RT_JMP2SK Jump   Exit=BS_RT_SKP
/Skip whitespace (space, tab, CR) but stop at LF or comment
NOLINK
ITO BS_RT_SKP    Jump    Exit=BS_RT_SKP_RD
NOLINK
ITO BS_RT_SKP_RD Jump    Exit=BS_RT_SKP_RD2  /alias: loop entry after byte read
NOLINK
NOP BS_RT_SKP_RD2
RVOCA BS_RT_BYTE  BS_READ_BYTE
/EOF?
JZ BS_RT_EOF RA_LOAD_BYTE BS_RT_DONE
/LF → end of line (return empty token, caller handles)
JEQ BS_RT_LF RA_LOAD_BYTE LF BS_RT_DONE
/slash → check for inline double-slash block or single-line comment
JEQ BS_RT_SL RA_LOAD_BYTE SLASH BS_RT_SL2
/space/tab/CR → continue skipping
SWITCH RA_LOAD_BYTE
    SP    BS_RT_SKP_RD2
    TAB   BS_RT_SKP_RD2
    CR    BS_RT_SKP_RD2
/Non-whitespace → start of token, fall through to collect
ITO BS_RT_COL    Jump    Exit=BS_RT_COLLECT
/First slash during skip: read second byte to distinguish '/' from double-slash
NOLINK
RVOCA BS_RT_SL2B BS_READ_BYTE
JEQ BS_RT_SL2CK RA_LOAD_BYTE SLASH BS_RT_BLKSK
/'/' alone (not double-slash) → single-line comment: skip to EOL, return empty token
RVOCA BS_RT_CSKP BS_SKIP_TO_EOL
ITO BS_RT_CDONE  Jump    Exit=BS_RT_DONE
/double-slash: skip inline block, then restart skip-loop to find next token
NOLINK
RVOCA BS_RT_BLK  BS_BLOCK_SKIP
ITO BS_RT_BLKDNE Jump    Exit=BS_RT_SKP
/Collect token bytes
NOLINK
ITO BS_RT_COLLECT Jump  Exit=BS_RT_COL2
NOLINK
ITO BS_RT_COL2   Add     El1=BS_TOKBUF_BASE El2=RA_LOAD_TLEN Exit=RA_BS_TMP
ITO BS_RT_STORE  Write   El1=RA_BS_TMP    El2=RA_LOAD_BYTE
/djb2: hash = hash * 33 + byte
ITO BS_RT_H33    Mul     El1=RA_LOAD_HASH   El2=C_33       Exit=RA_BS_TMP2
ITO BS_RT_HADD   Add     El1=RA_BS_TMP2   El2=RA_LOAD_BYTE Exit=RA_LOAD_HASH
ITO BS_RT_MASK   And     El1=RA_LOAD_HASH   El2=BS_HT_MASK Exit=RA_LOAD_HASH
ITO BS_RT_INC    Add     El1=RA_LOAD_TLEN   El2=C_1        Exit=RA_LOAD_TLEN
/Read next byte and check if still part of token
RVOCA BS_RT_NB   BS_READ_BYTE
SWITCH RA_LOAD_BYTE
    0     BS_RT_DONE
    SP    BS_RT_DONE
    TAB   BS_RT_DONE
    LF    BS_RT_DONE
    CR    BS_RT_DONE
    SLASH BS_RT_SLCK
ITO BS_RT_MORE   Jump    Exit=BS_RT_COL2
/'/' mid-token: read second byte to check for double-slash block
NOLINK
RVOCA BS_RT_SLRB BS_READ_BYTE
JEQ BS_RT_SLCK2 RA_LOAD_BYTE SLASH BS_RT_SLBL
/'/' alone mid-token: end token (single '/' is not inline comment mid-line)
ITO BS_RT_SLEND  Jump    Exit=BS_RT_DONE
/double-slash: end current token, skip inline block; next BS_READ_TOKEN call resumes after block
NOLINK
RVOCA BS_RT_SLBK BS_BLOCK_SKIP
ITO BS_RT_SLBND  Jump    Exit=BS_RT_DONE
NOLINK
RREDI BS_RT_DONE
/── BS_BLOCK_SKIP: skip bytes until double-slash found (inline block closer) ──────
/IN:  file positioned just after opening double-slash. Returns via RREDI (RA_LINK).
/Handles multi-line blocks; reads across LF boundaries until double-slash or EOF.
NOLINK
/Main read loop (entry point for loop-back jumps); linked from BS_BLOCK_SKIP via autolink
RVOCA BS_BLOCK_SKIP   BS_READ_BYTE
/EOF → done
JZ BS_BLK_EOF RA_LOAD_BYTE BS_BLK_DONE
/Found '/' → read next byte to check for double-slash
JEQ BS_BLK_SL RA_LOAD_BYTE SLASH BS_BLK_SL2
/Any other byte → continue loop
ITO BS_BLK_CONT   Jump   Exit=BS_BLOCK_SKIP
/First slash found: read next byte
NOLINK
RVOCA BS_BLK_SL2B BS_READ_BYTE
/Second slash → block closed
JEQ BS_BLK_SL2CK RA_LOAD_BYTE SLASH BS_BLK_DONE
/Not second slash → continue loop
ITO BS_BLK_LONE   Jump   Exit=BS_BLOCK_SKIP
/Done: automatic stack restores RA_LINK and returns
NOLINK
RREDI BS_BLK_DONE
/── BS_LOOKUP: find token in symbol table → RA_BS_RESULT ────────
/IN: RA_LOAD_HASH = hash, BS_TOKBUF = token bytes, RA_LOAD_TLEN = length
/OUT: RA_BS_RESULT = addr or 0
NOLINK
ITO BS_LOOKUP   Move    El1=RA_LOAD_HASH   Exit=RA_HT_HASH
ITO BS_LK_BASE  Move    El1=BS_HT_BASE     Exit=RA_HT_BASE
ITO BS_LK_MASK  Move    El1=BS_HT_MASK     Exit=RA_HT_MASK
ITO BS_LK_SIZE  Move    El1=BS_HT_SIZE     Exit=RA_HT_SIZE
RVOCA BS_LK_HT  HT_LOOKUP
ITO BS_LK_RES   Move    El1=RA_HT_RESULT   Exit=RA_BS_RESULT
RREDI BS_LK_RET

/── BS_INTERN: lookup or lazy-alloc token → RA_BS_RESULT ────────
/Plain variant: no name storage. Used for els, ops, misc tokens.
/If not found: alloc 1 lux (lux[0]=entry point), insert into htable.
NOLINK
RVOCA BS_INTERN  BS_LOOKUP
JZ BS_INT_CK RA_BS_RESULT BS_INT_ALLOC
RREDI BS_INT_DONE
/Alloc 1 lux: lux[0]=entry point only
NOLINK
ALLOC_TO BS_INT_ALLOC RA_BS_RESULT C_1
/Insert into htable
ITO BS_INT_HTINS Move   El1=RA_LOAD_HASH   Exit=RA_HT_HASH
ITO BS_INT_HTLID Move   El1=RA_BS_RESULT Exit=RA_HT_LID
ITO BS_INT_HTBAS Move   El1=BS_HT_BASE   Exit=RA_HT_BASE
ITO BS_INT_HTMSK Move   El1=BS_HT_MASK   Exit=RA_HT_MASK
ITO BS_INT_HTSIZ Move   El1=BS_HT_SIZE   Exit=RA_HT_SIZE
RVOCA BS_INT_HT  HT_INSERT
RREDI BS_INT_RETR

/── BS_INTERN_NAMED: like BS_INTERN but stores packed name in lux[1] ─
/Used for command tokens (first token of each line) so Python can rebuild
/the symbol table from aether. Allocs 2 luces.
NOLINK
RVOCA BS_INTERN_NAMED  BS_LOOKUP
JZ BS_INN_CK RA_BS_RESULT BS_INN_ALLOC
RREDI BS_INN_DONE
/Alloc 2 luces: lux[0]=entry point, lux[1]=name string addr
NOLINK
ITO BS_INN_ALLOC Move   El1=C_2          Exit=RA_ALLOC_COUNT
RVOCA BS_INN_AC  ALLOC_LUCES
/Save alloc addr in frame slot 1 (avoids global RA_BS_EL0; BS_PACK_TOKBUF clobbers RA_ALLOC_RESULT)
ITO BS_INN_SFS   Add    El1=RA_SP        El2=C_1              Exit=RA_CS_TMP
ITO BS_INN_SAV   Write  El1=RA_CS_TMP   El2=RA_ALLOC_RESULT
/Pack tokbuf → packed string; result addr in RA_BS_TMP2
RVOCA BS_INN_PACK BS_PACK_TOKBUF
/Restore intern lux addr from frame slot 1
ITO BS_INN_RFS   Add    El1=RA_SP        El2=C_1              Exit=RA_CS_TMP
ITO BS_INN_RST   Read   El1=RA_CS_TMP   Exit=RA_BS_RESULT
/Store packed string addr at lux[1]
ITO BS_INN_NAMESL Add   El1=RA_BS_RESULT El2=C_1          Exit=RA_BS_TMP
ITO BS_INN_NAMEW  Write El1=RA_BS_TMP    El2=RA_BS_TMP2
/Insert into htable
ITO BS_INN_HTINS Move   El1=RA_LOAD_HASH   Exit=RA_HT_HASH
ITO BS_INN_HTLID Move   El1=RA_BS_RESULT Exit=RA_HT_LID
ITO BS_INN_HTBAS Move   El1=BS_HT_BASE   Exit=RA_HT_BASE
ITO BS_INN_HTMSK Move   El1=BS_HT_MASK   Exit=RA_HT_MASK
ITO BS_INN_HTSIZ Move   El1=BS_HT_SIZE   Exit=RA_HT_SIZE
RVOCA BS_INN_HT  HT_INSERT
RREDI BS_INN_RETR

/── BS_PACK_TOKBUF: pack current token into dense packed string ─
/IN:  BS_TOKBUF_BASE (1 byte/lux), RA_LOAD_TLEN = token length
/OUT: RA_BS_TMP2 = addr of first packed lux (null-terminated, step=1)
/Clobbers: RA_BS_TMP, RA_BS_TMP3, RA_BS_PACK_WORD, RA_BS_PACK_SHIFT,
/          RA_BS_PACK_SIDX, RA_BS_PACK_DST
NOLINK
/Compute luces needed = ceil(tlen/8)+2: +1 for partial/flush, +1 for null terminator
ITO BS_PACK_TOKBUF Add    El1=RA_LOAD_TLEN El2=C_7        Exit=RA_BS_TMP
ITO BPT_LUCES2   Div    El1=RA_BS_TMP    El2=C_8        Exit=RA_ALLOC_COUNT
ITO BPT_LUCES3   Add    El1=RA_ALLOC_COUNT El2=C_2      Exit=RA_ALLOC_COUNT
RVOCA BPT_ALLOC  ALLOC_LUCES
/dst_base = RA_ALLOC_RESULT; save as output in RA_BS_TMP2
ITO BPT_SAVEDS   Move   El1=RA_ALLOC_RESULT Exit=RA_BS_TMP2
ITO BPT_INITDST  Move   El1=RA_ALLOC_RESULT Exit=RA_BS_PACK_DST
/Init loop state: sidx=0, shift=0, cur_word=0
CLEAR BPT_IS RA_BS_PACK_SIDX
CLEAR BPT_IW RA_BS_PACK_WORD
CLEAR BPT_ISH RA_BS_PACK_SHIFT
/Loop: check sidx >= tlen → flush
JEQ BPT_LOOP RA_BS_PACK_SIDX RA_LOAD_TLEN BPT_FLUSH
/Load byte from tokbuf: a[BS_TOKBUF_BASE + sidx]
ITO BPT_BADDR    Add    El1=BS_TOKBUF_BASE El2=RA_BS_PACK_SIDX Exit=RA_BS_TMP
ITO BPT_BLOAD    Read   El1=RA_BS_TMP    Exit=RA_BS_TMP3
/Shift byte into position: shifted = byte << shift
ITO BPT_BSHIFT   Left   El1=RA_BS_TMP3  El2=RA_BS_PACK_SHIFT Exit=RA_BS_TMP3
/OR into cur_word (no overlap since shift advances by 8 each time)
ITO BPT_BOR      Add    El1=RA_BS_PACK_WORD El2=RA_BS_TMP3 Exit=RA_BS_PACK_WORD
/sidx++; shift+=8
ITO BPT_SIDINC   Add    El1=RA_BS_PACK_SIDX El2=C_1      Exit=RA_BS_PACK_SIDX
ITO BPT_SHINC    Add    El1=RA_BS_PACK_SHIFT El2=C_8     Exit=RA_BS_PACK_SHIFT
/if shift == 64: flush cur_word to dst, advance dst, reset
JEQ BPT_FULL     RA_BS_PACK_SHIFT C_64 BPT_FLUSH_WORD
ITO BPT_LBJMP    Jump   Exit=BPT_LOOP
/Flush cur_word to dst and reset
NOLINK
ITO BPT_FLUSH_WORD Write El1=RA_BS_PACK_DST El2=RA_BS_PACK_WORD
ITO BPT_DSTINC   Add   El1=RA_BS_PACK_DST El2=C_1       Exit=RA_BS_PACK_DST
CLEAR BPT_WCLR RA_BS_PACK_WORD
CLEAR BPT_SCLR RA_BS_PACK_SHIFT
ITO BPT_FWJMP    Jump  Exit=BPT_LOOP
/Flush: write remaining partial word (may be 0 if tlen%8==0, still write)
/then write null terminator
NOLINK
ITO BPT_FLUSH    Write  El1=RA_BS_PACK_DST El2=RA_BS_PACK_WORD
ITO BPT_NULLINC  Add    El1=RA_BS_PACK_DST El2=C_1      Exit=RA_BS_PACK_DST
CLEAR BPT_NULL RA_BS_TMP3
ITO BPT_NULLW    Write  El1=RA_BS_PACK_DST El2=RA_BS_TMP3
/restore RA_LINK and return; RA_BS_TMP2 holds result
RREDI BPT_RRET

/── BS_PROCESS_FILE_NEW: sweep A — process one file, NEW only ───
/Reads lines; for each: reads first token, if "NEW"→ intern name, else skip line.
NOLINK
ITO BS_PROCESS_FILE_NEW Jump  Exit=BS_PFN_LINE
NOLINK
ITO BS_PFN_LINE  Jump   Exit=BS_PFN_L2
NOLINK
RVOCA BS_PFN_L2   BS_READ_TOKEN
/EOF → done
JZ BS_PFN_EOFCK RA_LOAD_BYTE BS_PFN_DONE
/Empty token (LF) → next line
JZ BS_PFN_EMCK RA_LOAD_TLEN BS_PFN_LINE
/Check if first token is "NEW" (3 bytes: N=78, E=69, W=87)
ITO BS_PFN_T0    Read   El1=BS_TOKBUF_BASE            Exit=RA_BS_TMP
JEQ BS_PFN_N RA_BS_TMP ASCII_N BS_PFN_MAYBE_NEW
RVOCA BS_PFN_SKL BS_SKIP_TO_EOL
ITO BS_PFN_SKJMP Jump   Exit=BS_PFN_LINE
/Maybe NEW: check length == 3 and E,W
NOLINK
JEQ BS_PFN_MAYBE_NEW RA_LOAD_TLEN C_3 BS_PFN_CHK_E
RVOCA BS_PFN_SKL2 BS_SKIP_TO_EOL
ITO BS_PFN_SKJMP2 Jump  Exit=BS_PFN_LINE
NOLINK
ITO BS_PFN_CHK_E Add    El1=BS_TOKBUF_BASE El2=C_1      Exit=RA_BS_TMP
ITO BS_PFN_RE    Read   El1=RA_BS_TMP    Exit=RA_BS_TMP
JEQ BS_PFN_CE RA_BS_TMP ASCII_E BS_PFN_CHK_W
RVOCA BS_PFN_SKL3 BS_SKIP_TO_EOL
ITO BS_PFN_SKJMP3 Jump  Exit=BS_PFN_LINE
NOLINK
ITO BS_PFN_CHK_W Add    El1=BS_TOKBUF_BASE El2=C_2      Exit=RA_BS_TMP
ITO BS_PFN_RW    Read   El1=RA_BS_TMP    Exit=RA_BS_TMP
JEQ BS_PFN_CW RA_BS_TMP ASCII_W BS_PFN_IS_NEW
RVOCA BS_PFN_SKL4 BS_SKIP_TO_EOL
ITO BS_PFN_SKJMP4 Jump  Exit=BS_PFN_LINE
/It IS "NEW" → read name token, intern it
NOLINK
RVOCA BS_PFN_NTNM BS_READ_TOKEN
JZ BS_PFN_NTCK RA_LOAD_TLEN BS_PFN_LINE
RVOCA BS_PFN_INTR BS_INTERN
/Skip rest of line (NEW has no more els)
RVOCA BS_PFN_SKLN BS_SKIP_TO_EOL
ITO BS_PFN_CONT  Jump   Exit=BS_PFN_LINE
NOLINK
RREDI BS_PFN_DONE
/── BS_PROCESS_FILE_WIRE: sweep B — process one file, wire all──
/For each line: read command token, dispatch.
/NEW → skip (already done). SET/LINK → handle. other → {CMD}_MACRO.
NOLINK
ITO BS_PROCESS_FILE_WIRE Jump  Exit=BS_PFW_LINE
NOLINK
ITO BS_PFW_LINE  Jump   Exit=BS_PFW_L2
NOLINK
RVOCA BS_PFW_L2   BS_READ_TOKEN
JZ BS_PFW_EOFCK RA_LOAD_BYTE BS_PFW_DONE
JZ BS_PFW_EMCK RA_LOAD_TLEN BS_PFW_LINE
/Intern the command to get its addr; use addr to find {CMD}_MACRO
/We pass command addr in RA_MA0 and let BS_DISPATCH handle it
RVOCA BS_PFW_INT BS_INTERN
ITO BS_PFW_CMD   Move   El1=RA_BS_RESULT Exit=RA_MA0
/Skip NEW lines
/Actually we must still skip the rest of line for NEW
/Dispatch via BS_DISPATCH
RVOCA BS_PFW_DIS BS_DISPATCH
ITO BS_PFW_CONT  Jump   Exit=BS_PFW_LINE
NOLINK
RREDI BS_PFW_DONE
/── BS_DISPATCH: dispatch command token → handler ───────────────
/IN: RA_MA0 = cmd addr (already interned), command bytes in BS_TOKBUF
/Reads up to 3 els as needed. Dispatches by first byte of command.
/NEW → skip line. SET → BS_DO_SET. LINK → BS_DO_LINK. else → BS_DO_MACRO.
NOLINK
/Read first byte of token to dispatch
ITO BS_DISPATCH  Read   El1=BS_TOKBUF_BASE Exit=RA_BS_TMP
/N → NEW: skip line
JEQ BS_DIS_N RA_BS_TMP ASCII_N BS_DIS_NEW
/S → SET/SETREF/etc
JEQ BS_DIS_S RA_BS_TMP ASCII_S BS_DIS_SET
/L → LINK
JEQ BS_DIS_L RA_BS_TMP ASCII_L BS_DIS_LINK
/Anything else → macro dispatch
RVOCA BS_DIS_MJ  BS_DO_MACRO
ITO BS_DIS_DONE  Jump   Exit=BS_DIS_RET
/NEW: skip rest of line
NOLINK
RVOCA BS_DIS_NSK BS_SKIP_TO_EOL
ITO BS_DIS_NDONE Jump   Exit=BS_DIS_RET
/SET family: delegate to BS_DO_SET
NOLINK
RVOCA BS_DIS_SFN BS_DO_SET
ITO BS_DIS_SDONE Jump   Exit=BS_DIS_RET
/LINK: delegate to BS_DO_LINK
NOLINK
RVOCA BS_DIS_LFN BS_DO_LINK
RREDI BS_DIS_RET
/── BS_DO_SET: handle SET/NEWSET/NEWREF/SETREF ─────────────────
/Reads name + value tokens, resolves, writes aether[name] = value.
NOLINK
/Read name
RVOCA BS_DO_SET  BS_READ_TOKEN
RVOCA BS_DS_NI   BS_INTERN
ITO BS_DS_NADDR  Move   El1=RA_BS_RESULT Exit=RA_BS_EL0
/Read value
RVOCA BS_DS_VL   BS_READ_TOKEN
/Try integer parse first (check first byte is digit or '-')
ITO BS_DS_VLCK   Read   El1=BS_TOKBUF_BASE Exit=RA_BS_TMP
/If it's a digit (48-57) or '-' (45) → parse as integer
JEQ BS_DS_DASH RA_BS_TMP MINUS BS_DS_INTVAL
ITO BS_DS_DIG0   ULess  El1=RA_BS_TMP    El2=ASCII_0      Exit=RA_BS_FLAG
ITO BS_DS_DIG0J  JumpIf El1=RA_BS_FLAG   Exit=BS_DS_SYMVAL
ITO BS_DS_DIG9   ULess  El1=ASCII_9      El2=RA_BS_TMP    Exit=RA_BS_FLAG
ITO BS_DS_DIG9J  JumpIf El1=RA_BS_FLAG   Exit=BS_DS_SYMVAL
/Integer: parse from tokbuf
NOLINK
RVOCA BS_DS_PINT BS_PARSE_INT
/Write value: aether[aether[RA_BS_EL0]] = parsed_int
ITO BS_DS_WADDR  Read   El1=RA_BS_EL0   Exit=RA_BS_TMP
ITO BS_DS_WRITE  Write  El1=RA_BS_TMP    El2=RA_BS_TMP2   /TMP2 = result from BS_PARSE_INT
RVOCA BS_DS_SKL  BS_SKIP_TO_EOL
RREDI BS_DS_RRET
/Symbol value: intern and use addr
NOLINK
RVOCA BS_DS_SYMVAL   BS_INTERN
/aether[aether[RA_BS_EL0]] = RA_BS_RESULT
ITO BS_DS_SWADDR Read   El1=RA_BS_EL0   Exit=RA_BS_TMP
ITO BS_DS_SWRITE Write  El1=RA_BS_TMP    El2=RA_BS_RESULT
RVOCA BS_DS_SSKL BS_SKIP_TO_EOL
RREDI BS_DS_SRRET

/── BS_DO_LINK: handle LINK src rel exit ────────────────────────
/Reads 3 name tokens, writes lumen pair (rel_addr, tgt_addr) after src.word.
/Uses ADD_LUMEN — the canonical lumen append function.
NOLINK
RVOCA BS_DO_LINK  BS_READ_TOKEN
RVOCA BS_DL_SI   BS_INTERN
ITO BS_DL_SRC    Move   El1=RA_BS_RESULT Exit=RA_LM_SRC
RVOCA BS_DL_R    BS_READ_TOKEN
RVOCA BS_DL_RI   BS_INTERN
ITO BS_DL_REL    Move   El1=RA_BS_RESULT Exit=RA_LM_REL
RVOCA BS_DL_T    BS_READ_TOKEN
RVOCA BS_DL_TI   BS_INTERN
ITO BS_DL_EXIT_N Move   El1=RA_BS_RESULT Exit=RA_LM_EXIT
/Append lumen pair via ADD_LUMEN (handles overflow chain, correct lumen model)
RVOCA BS_DL_ADD  ADD_LUMEN
RVOCA BS_DL_SKL  BS_SKIP_TO_EOL
RREDI BS_DL_RRET

/── BS_DO_MACRO: look up {CMD}_MACRO and call ──────────────────
/IN: RA_MA0 = cmd addr. Reads remaining tokens as MA1..MA7.
/Looks up {CMD}_MACRO by appending "_MACRO" suffix to cmd name,
/hashing, looking up. If found: sets RA_MC_PREV=_last_ito, calls.
NOLINK
/Read remaining els into MA1..MA7
CLEAR BS_DO_MACRO RA_BS_ELC
RVOCA BS_DM_ARGS BS_READ_ARGS_KV
/CMD addr is already in RA_MA0. Use it directly as the program to call.
/RA_MA0 = addr of cmd lux. aether[RA_MA0] = cmd addr itself (self-ref if aspect,
/or addr of first ITO of the program if it's a Reca subroutine).
JZ BS_DM_CK RA_MA0 BS_DM_SKIP
/Found: set RA_MC_PREV to _last_ito (stored in BS_LAST_ITO), call macro
ITO BS_DM_PREV   Move   El1=BS_LAST_ITO  Exit=RA_MC_PREV
RVOCA BS_DM_CALL BS_CALL_MACRO
/After call: sync RA_MC_PREV → BS_LAST_ITO
ITO BS_DM_SYNC   Move   El1=RA_MC_PREV   Exit=BS_LAST_ITO
RVOCA BS_DM_SKL  BS_SKIP_TO_EOL
RREDI BS_DM_RRET
NOLINK
RVOCA BS_DM_SSKL BS_SKIP_TO_EOL
RREDI BS_DM_SKIP
/── Supporting state ───────────────────────────────────────────
NEW BS_LAST_ITO  /tracks last ITO lux (like Python _last_ito)

/── BS_PARSE_INT: parse integer from BS_TOKBUF → RA_BS_TMP2 ───
/Simple: iterate bytes, accumulate. Handles '-' prefix.
NOLINK
CLEAR BS_PARSE_INT RA_BS_TMP2
CLEAR BS_PI_IDX RA_BS_TMP3
CLEAR BS_PI_NEG RA_BS_FLAG
/Check for '-'
ITO BS_PI_RB0    Read   El1=BS_TOKBUF_BASE Exit=RA_BS_TMP
JEQ BS_PI_DCHK RA_BS_TMP MINUS BS_PI_NEG_SET
ITO BS_PI_START  Jump   Exit=BS_PI_LOOP
NOLINK
ITO BS_PI_NEG_SET Move  El1=C_1          Exit=RA_BS_FLAG
ITO BS_PI_NIDX   Add    El1=RA_BS_TMP3   El2=C_1          Exit=RA_BS_TMP3  /skip the dash: idx=1
ITO BS_PI_LOOP2  Jump   Exit=BS_PI_LOOP
NOLINK
JEQ BS_PI_LOOP RA_BS_TMP3 RA_LOAD_TLEN BS_PI_DONE
ITO BS_PI_BADDR  Add    El1=BS_TOKBUF_BASE El2=RA_BS_TMP3  Exit=RA_BS_TMP
ITO BS_PI_BYTE   Read   El1=RA_BS_TMP    Exit=RA_BS_TMP
/Check byte is in '0'..'9' range before processing
ITO BS_PI_DCK0   ULess  El1=RA_BS_TMP    El2=ASCII_0      Exit=RA_BS_FLAG
ITO BS_PI_DCK0J  JumpIf El1=RA_BS_FLAG   Exit=BS_PI_DONE
ITO BS_PI_DCK9   ULess  El1=ASCII_9      El2=RA_BS_TMP    Exit=RA_BS_FLAG
ITO BS_PI_DCK9J  JumpIf El1=RA_BS_FLAG   Exit=BS_PI_DONE
ITO BS_PI_SUB0   Sub    El1=RA_BS_TMP    El2=ASCII_0      Exit=RA_BS_TMP
ITO BS_PI_MUL    Mul    El1=RA_BS_TMP2   El2=C_10         Exit=RA_BS_TMP2
ITO BS_PI_ADD    Add    El1=RA_BS_TMP2   El2=RA_BS_TMP    Exit=RA_BS_TMP2
ITO BS_PI_INC    Add    El1=RA_BS_TMP3   El2=C_1          Exit=RA_BS_TMP3
ITO BS_PI_JMP    Jump   Exit=BS_PI_LOOP
NOLINK
ITO BS_PI_DONE   JumpIf El1=RA_BS_FLAG   Exit=BS_PI_NEGATE
RREDI BS_PI_OKRET
NOLINK
ITO BS_PI_NEGATE Sub    El1=C_0          El2=RA_BS_TMP2   Exit=RA_BS_TMP2
RREDI BS_PI_NRRET

/── BS_READ_ARGS: read tokens into MA1..MA7 ────────────────────
/Reads until LF/EOF. Interns each. Writes into RA_MA1..RA_MA7.
NOLINK
/Read up to 7 els (MA1..MA7)
/Simplified: read one token, intern, write to MA1
RVOCA BS_READ_ARGS BS_READ_TOKEN
JZ BS_RA_CK1 RA_LOAD_TLEN BS_RA_DONE
JZ BS_RA_LF1 RA_LOAD_BYTE BS_RA_DONE
RVOCA BS_RA_I1   BS_INTERN
ITO BS_RA_W1     Move   El1=RA_BS_RESULT Exit=RA_MA1
RVOCA BS_RA_T2   BS_READ_TOKEN
JZ BS_RA_CK2 RA_LOAD_TLEN BS_RA_DONE
RVOCA BS_RA_I2   BS_INTERN
ITO BS_RA_W2     Move   El1=RA_BS_RESULT Exit=RA_MA2
RVOCA BS_RA_T3   BS_READ_TOKEN
JZ BS_RA_CK3 RA_LOAD_TLEN BS_RA_DONE
RVOCA BS_RA_I3   BS_INTERN
ITO BS_RA_W3     Move   El1=RA_BS_RESULT Exit=RA_MA3
RVOCA BS_RA_T4   BS_READ_TOKEN
JZ BS_RA_CK4 RA_LOAD_TLEN BS_RA_DONE
RVOCA BS_RA_I4   BS_INTERN
ITO BS_RA_W4     Move   El1=RA_BS_RESULT Exit=RA_MA4
RVOCA BS_RA_T5   BS_READ_TOKEN
JZ BS_RA_CK5 RA_LOAD_TLEN BS_RA_DONE
RVOCA BS_RA_I5   BS_INTERN
ITO BS_RA_W5     Move   El1=RA_BS_RESULT Exit=RA_MA5
RVOCA BS_RA_T6   BS_READ_TOKEN
JZ BS_RA_CK6 RA_LOAD_TLEN BS_RA_DONE
RVOCA BS_RA_I6   BS_INTERN
ITO BS_RA_W6     Move   El1=RA_BS_RESULT Exit=RA_MA6
RVOCA BS_RA_T7   BS_READ_TOKEN
JZ BS_RA_CK7 RA_LOAD_TLEN BS_RA_DONE
RVOCA BS_RA_I7   BS_INTERN
ITO BS_RA_W7     Move   El1=RA_BS_RESULT Exit=RA_MA7
ITO BS_RA_W7_JMP2BS_RA_DO   Jump  Exit=BS_RA_DONE
NOLINK
RREDI BS_RA_DONE
/── BS_CALL_MACRO: call the Reca macro at RA_BS_RESULT ─────────
/RA_MA0..RA_MA7 are already set. RA_MC_PREV is set by caller.
NOLINK
/Call the program: RA_MA0 holds addr of cmd lux, aether[RA_MA0] = entry point
ITO BS_CALL_MACRO Read   El1=RA_MA0       Exit=RA_BS_TMP
ITO BS_CM_CALL   Voca   El1=RA_BS_TMP    Exit=RA_LINK
RREDI BS_CM_RRET

/── BS_TOKEN_VALUE: extract value from "key=value" token ────────
/If BS_TOKBUF contains '=', return the part after it as a new token hash+content.
/If no '=', treat whole token as value. Result: RA_LOAD_HASH updated for value part.
/Modifies BS_TOKBUF in-place to contain only the value part.
NOLINK
CLEAR BS_TOKEN_VALUE RA_BS_TMP3
/Scan for '='
ITO BS_TV_IDX_JMP2BS_TV_LO   Jump  Exit=BS_TV_LOOP
NOLINK
JEQ BS_TV_LOOP RA_BS_TMP3 RA_LOAD_TLEN BS_TV_NOEQ
ITO BS_TV_BADDR    Add    El1=BS_TOKBUF_BASE El2=RA_BS_TMP3  Exit=RA_BS_TMP
ITO BS_TV_BYTE     Read   El1=RA_BS_TMP    Exit=RA_BS_TMP
JEQ BS_TV_EQC RA_BS_TMP ASCII_EQ BS_TV_FOUND
ITO BS_TV_INC      Add    El1=RA_BS_TMP3   El2=C_1          Exit=RA_BS_TMP3
ITO BS_TV_JMP      Jump   Exit=BS_TV_LOOP
/No '=': whole token is value, return as-is
NOLINK
RREDI BS_TV_NOEQ
/Found '=': shift tokbuf left from pos+1, recompute hash
NOLINK
ITO BS_TV_FOUND    Add    El1=RA_BS_TMP3   El2=C_1          Exit=RA_BS_TMP3
/RA_BS_TMP3 = start of value part. Copy value to front of tokbuf.
CLEAR BS_TV_VIDX RA_BS_TMP2
ITO BS_TV_VIDX_JMP2BS_TV_CL   Jump  Exit=BS_TV_CLOOP
NOLINK
JEQ BS_TV_CLOOP RA_BS_TMP3 RA_LOAD_TLEN BS_TV_CDONE
ITO BS_TV_CSRC     Add    El1=BS_TOKBUF_BASE El2=RA_BS_TMP3  Exit=RA_BS_TMP
ITO BS_TV_CLOAD    Read   El1=RA_BS_TMP    Exit=RA_BS_TMP
ITO BS_TV_CDST     Add    El1=BS_TOKBUF_BASE El2=RA_BS_TMP2  Exit=RA_BS_FLAG
ITO BS_TV_CSTO     Write  El1=RA_BS_FLAG   El2=RA_BS_TMP
ITO BS_TV_CINC1    Add    El1=RA_BS_TMP3   El2=C_1          Exit=RA_BS_TMP3
ITO BS_TV_CINC2    Add    El1=RA_BS_TMP2   El2=C_1          Exit=RA_BS_TMP2
ITO BS_TV_CJMP     Jump   Exit=BS_TV_CLOOP
NOLINK
ITO BS_TV_CDONE    Move   El1=RA_BS_TMP2   Exit=RA_LOAD_TLEN
/Recompute hash for new content
CLEAR BS_TV_HIDX RA_BS_TMP2
ITO BS_TV_HASH0    Move   El1=BS_HASH0     Exit=RA_LOAD_HASH
ITO BS_TV_HASH0_JMP2BS_TV_HL   Jump  Exit=BS_TV_HLOOP
NOLINK
JEQ BS_TV_HLOOP RA_BS_TMP2 RA_LOAD_TLEN BS_TV_HDONE
ITO BS_TV_HBADDR   Add    El1=BS_TOKBUF_BASE El2=RA_BS_TMP2  Exit=RA_BS_TMP
ITO BS_TV_HBYTE    Read   El1=RA_BS_TMP    Exit=RA_BS_TMP
ITO BS_TV_HM33     Mul    El1=RA_LOAD_HASH   El2=C_33          Exit=RA_BS_TMP3
ITO BS_TV_HADD     Add    El1=RA_BS_TMP3   El2=RA_BS_TMP     Exit=RA_LOAD_HASH
ITO BS_TV_HMASK    And    El1=RA_LOAD_HASH   El2=BS_HT_MASK    Exit=RA_LOAD_HASH
ITO BS_TV_HINC     Add    El1=RA_BS_TMP2   El2=C_1           Exit=RA_BS_TMP2
ITO BS_TV_HJMP     Jump   Exit=BS_TV_HLOOP
NOLINK
RREDI BS_TV_HDONE
/── ITO program: handles ITO name op [El1=x] [El2=y] [Exit=z] ──
/MA0=name_addr. Reads op+els from current token position.
/Allocs 7 luces, sets self-ref, writes op/e1/e2/exit slots.
/Called by bootstrap.re dispatch when command == "ITO".
NOLINK
/Read op token
RVOCA BS_DO_ITO     BS_READ_TOKEN
RVOCA BS_DI_OPV     BS_TOKEN_VALUE
RVOCA BS_DI_OPI     BS_INTERN
ITO BS_DI_OPADDR    Move  El1=RA_BS_RESULT Exit=RA_BS_EL0
/Alloc 7 luces for ITO lux
ALLOC_TO BS_DI_CNT RA_BS_EL1 ITO_SIZE
/Set self-ref: aether[addr] = addr
ITO BS_DI_SELF      Write El1=RA_ALLOC_RESULT El2=RA_ALLOC_RESULT
/Write op at slot 1
ITO BS_DI_OPS       Add   El1=RA_BS_EL1   El2=C_1           Exit=RA_BS_TMP
ITO BS_DI_OPW       Write El1=RA_BS_TMP    El2=RA_BS_EL0
/Register name: MA0 = name_addr, write addr into it
ITO BS_DI_NAMEW     Write El1=RA_MA0       El2=RA_BS_EL1
/Read remaining key=value els (El1, El2, Exit)
CLEAR BS_DI_E1 RA_BS_EL2
CLEAR BS_DI_E2D RA_BS_EL3
RVOCA BS_DI_ARGS    BS_DO_ITO_ARGS
/Write e1 at slot 2
ITO BS_DI_E1S       Add   El1=RA_BS_EL1   El2=C_2           Exit=RA_BS_TMP
ITO BS_DI_E1W       Write El1=RA_BS_TMP    El2=RA_BS_EL2
/Write e2 at slot 3 — use RA_BS_TMP2 for e2
ITO BS_DI_E2S       Add   El1=RA_BS_EL1   El2=C_3           Exit=RA_BS_TMP
ITO BS_DI_E2W       Write El1=RA_BS_TMP    El2=RA_BS_TMP2
/Write exit at slot 4
ITO BS_DI_EXS       Add   El1=RA_BS_EL1   El2=C_4           Exit=RA_BS_TMP
ITO BS_DI_EXW       Write El1=RA_BS_TMP    El2=RA_BS_EL3
/Autolink: if BS_LAST_ITO != 0, write addr into last_ito.next (slot 5)
JZ BS_DI_LCK BS_LAST_ITO BS_DI_NOLINK
ITO BS_DI_LNXT      Add   El1=BS_LAST_ITO  El2=C_5           Exit=RA_BS_TMP
ITO BS_DI_LW        Write El1=RA_BS_TMP    El2=RA_BS_EL1
ITO BS_DI_LW_JMP2BS_DI_NO   Jump  Exit=BS_DI_NOLINK
NOLINK
ITO BS_DI_NOLINK    Move  El1=RA_BS_EL1   Exit=BS_LAST_ITO
RVOCA BS_DI_SKL     BS_SKIP_TO_EOL
RREDI BS_DI_RRET

/We need RA_BS_EL3 for exit slot
NEW RA_BS_EL3

/── BS_DO_ITO_ARGS: read El1=/El2=/Exit= tokens, fill ARG2/TMP2/ARG3 ──
NOLINK
ITO BS_DO_ITO_ARGS Jump  Exit=BS_DIA_LOOP
NOLINK
JEQ BS_DIA_LOOP RA_LOAD_BYTE LF BS_DIA_DONE
JZ BS_DIA_EOFCK RA_LOAD_BYTE BS_DIA_DONE
RVOCA BS_DIA_TOK    BS_READ_TOKEN
JZ BS_DIA_EMCK RA_LOAD_TLEN BS_DIA_DONE
/Check first 2 bytes for El/Ex key
ITO BS_DIA_B0       Read  El1=BS_TOKBUF_BASE                  Exit=RA_BS_TMP
ITO BS_DIA_B1A      Add   El1=BS_TOKBUF_BASE El2=C_1          Exit=RA_BS_TMP3
ITO BS_DIA_B1       Read  El1=RA_BS_TMP3   Exit=RA_BS_TMP3
/E+l → El1 or El2
JEQ BS_DIA_EL RA_BS_TMP3 ASCII_ll BS_DIA_EL
/E+x → Exit
JEQ BS_DIA_EX RA_BS_TMP3 ASCII_xl BS_DIA_EX
ITO BS_DIA_CONT     Jump  Exit=BS_DIA_LOOP
/El: check digit after El
NOLINK
ITO BS_DIA_EL       Add   El1=BS_TOKBUF_BASE El2=C_2          Exit=RA_BS_TMP
ITO BS_DIA_ELD      Read  El1=RA_BS_TMP    Exit=RA_BS_TMP
RVOCA BS_DIA_ELV    BS_TOKEN_VALUE
RVOCA BS_DIA_ELI    BS_INTERN
/digit '1'?
JEQ BS_DIA_D1 RA_BS_TMP ASCII_1 BS_DIA_SET1
ITO BS_DIA_SET2     Move  El1=RA_BS_RESULT Exit=RA_BS_TMP2
ITO BS_DIA_C2       Jump  Exit=BS_DIA_LOOP
NOLINK
ITO BS_DIA_SET1     Move  El1=RA_BS_RESULT Exit=RA_BS_EL2
ITO BS_DIA_C1       Jump  Exit=BS_DIA_LOOP
/Exit:
NOLINK
RVOCA BS_DIA_EXV    BS_TOKEN_VALUE
RVOCA BS_DIA_EXI    BS_INTERN
ITO BS_DIA_SETEX    Move  El1=RA_BS_RESULT Exit=RA_BS_EL3
ITO BS_DIA_CXJMP    Jump  Exit=BS_DIA_LOOP
NOLINK
RREDI BS_DIA_DONE
/── BS_READ_ARGS_KV: read tokens, route key=value by key, else positional ──
/Key→MA slot: El1→MA2, El2→MA3, Exit→MA4, Next→MA5. Others: MA1..MA7 positional.
/After call: MA1..MA7 set. Stops at LF/EOF.
NEW RA_BS_POS_SLOT          /next positional slot index (1..7)
NOLINK
ITO BS_READ_ARGS_KV  Move   El1=C_1          Exit=RA_BS_POS_SLOT
/Zero MA1..MA7
FOR RA_MA1 RA_MA2 RA_MA3 RA_MA4 RA_MA5 RA_MA6 RA_MA7
    CLEAR BS_RAKV_Z{N} {X}
ITO BS_RAKV_Z7_JMP2BS_RAKV_   Jump  Exit=BS_RAKV_LOOP
NOLINK
JEQ BS_RAKV_LOOP RA_LOAD_BYTE LF BS_RAKV_DONE
JZ BS_RAKV_EOFCK RA_LOAD_BYTE BS_RAKV_DONE
RVOCA BS_RAKV_TOK    BS_READ_TOKEN
JZ BS_RAKV_EMCK RA_LOAD_TLEN BS_RAKV_DONE
/Check for '=' in token via BS_TOKEN_VALUE — it strips key, leaves value
/But we need the key too. Check first bytes manually.
/Check if token contains '=': scan for it
RVOCA BS_RAKV_SCAN   BS_SCAN_EQ
JZ BS_RAKV_EQCK RA_BS_FLAG BS_RAKV_POS
/Has '=': check key (first byte pair)
ITO BS_RAKV_KB0      Read   El1=BS_TOKBUF_BASE                  Exit=RA_BS_TMP
/E=69?
JEQ BS_RAKV_KE RA_BS_TMP ASCII_E BS_RAKV_EKEY
ITO BS_RAKV_KUNK     Jump   Exit=BS_RAKV_POS  /unknown key, treat positional
/E* key: check second byte
NOLINK
ITO BS_RAKV_EKEY     Add    El1=BS_TOKBUF_BASE El2=C_1          Exit=RA_BS_TMP
ITO BS_RAKV_KB1      Read   El1=RA_BS_TMP    Exit=RA_BS_TMP
/l→El1/El2, x→Exit
JEQ BS_RAKV_EL RA_BS_TMP ASCII_ll BS_RAKV_EL
JEQ BS_RAKV_EX RA_BS_TMP ASCII_xl BS_RAKV_EXIT
ITO BS_RAKV_EUNK     Jump   Exit=BS_RAKV_POS
/El*: check digit
NOLINK
ITO BS_RAKV_EL       Add    El1=BS_TOKBUF_BASE El2=C_2          Exit=RA_BS_TMP
ITO BS_RAKV_ELD      Read   El1=RA_BS_TMP    Exit=RA_BS_TMP
RVOCA BS_RAKV_ELV    BS_TOKEN_VALUE    /strips key=, leaves value in tokbuf
RVOCA BS_RAKV_ELI    BS_INTERN
JEQ BS_RAKV_EL1 RA_BS_TMP ASCII_1 BS_RAKV_SET2
ITO BS_RAKV_SET3     Move   El1=RA_BS_RESULT Exit=RA_MA3
ITO BS_RAKV_C3       Jump   Exit=BS_RAKV_LOOP
NOLINK
ITO BS_RAKV_SET2     Move   El1=RA_BS_RESULT Exit=RA_MA2
ITO BS_RAKV_C2       Jump   Exit=BS_RAKV_LOOP
/Exit:
NOLINK
RVOCA BS_RAKV_EXITV  BS_TOKEN_VALUE
RVOCA BS_RAKV_EXITI  BS_INTERN
ITO BS_RAKV_SET4     Move   El1=RA_BS_RESULT Exit=RA_MA4
ITO BS_RAKV_C4J      Jump   Exit=BS_RAKV_LOOP
/Positional: write to MA[pos_slot]
NOLINK
RVOCA BS_RAKV_POSV   BS_TOKEN_VALUE
RVOCA BS_RAKV_POSI   BS_INTERN
/Write result to RA_MA[pos_slot] — use SWITCH on pos_slot
SWITCH RA_BS_POS_SLOT
    1 BS_RAKV_W0
    2 BS_RAKV_W1
    3 BS_RAKV_W2
    4 BS_RAKV_W3
    5 BS_RAKV_W4
    6 BS_RAKV_W5
    7 BS_RAKV_W6
/Fallthrough: pos_slot > 7 — all slots filled, continue consuming tokens until LF/EOF
ITO BS_RAKV_FALL Jump Exit=BS_RAKV_LOOP
FOR RA_MA1 RA_MA2 RA_MA3 RA_MA4 RA_MA5 RA_MA6 RA_MA7
    NOLINK
    ITO BS_RAKV_W{N}    Move El1=RA_BS_RESULT    Exit={X}
    ITO BS_RAKV_INC{N}  Add  El1=RA_BS_POS_SLOT  El2=C_1  Exit=RA_BS_POS_SLOT
    ITO BS_RAKV_J{N}    Jump Exit=BS_RAKV_LOOP
NOLINK
RREDI BS_RAKV_DONE
/── BS_SCAN_EQ: scan BS_TOKBUF for '=', set RA_BS_FLAG=1 if found ──
NOLINK
CLEAR BS_SCAN_EQ RA_BS_TMP3
ITO BS_SE2_START Jump  Exit=BS_SE2_LOOP
NOLINK
JEQ BS_SE2_LOOP RA_BS_TMP3 RA_LOAD_TLEN BS_SE2_NF
ITO BS_SE2_ADDR  Add    El1=BS_TOKBUF_BASE El2=RA_BS_TMP3  Exit=RA_BS_TMP
ITO BS_SE2_BYTE  Read   El1=RA_BS_TMP    Exit=RA_BS_TMP
JEQ BS_SE2_EQC RA_BS_TMP ASCII_EQ BS_SE2_FOUND
ITO BS_SE2_INC   Add    El1=RA_BS_TMP3   El2=C_1           Exit=RA_BS_TMP3
ITO BS_SE2_JMP   Jump   Exit=BS_SE2_LOOP
NOLINK
ITO BS_SE2_FOUND Move   El1=C_1          Exit=RA_BS_FLAG
RREDI BS_SE2_FRRET
NOLINK
CLEAR BS_SE2_NF RA_BS_FLAG
RREDI BS_SE2_NRRET
