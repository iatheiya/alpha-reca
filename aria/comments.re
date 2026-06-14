//comments.re — Block comment delimiter declaration + stripping protocol

/TWO SEPARATE CONCERNS:

/   1. DECLARATION — what the delimiter IS.
/        COMMENT_OPEN and COMMENT_CLOSE are Lux carrying the delimiter string.
/        This is the canonical definition. Any tool that strips comments reads
/        these Lux to know what to look for.

/   2. STRIPPING — a process that consumes a byte stream and emits a byte
/      stream with (open) ... (close) regions removed. Described below as an aria.
/      The Python bootstrap (loader.py _read_re_file) is a DUPLICATE of
/      this Aria — it exists only because bootstrap cannot run Reca before
/      Reca is loaded (chicken-and-egg). The canonical behaviour lives here.

/ DELIMITER: (open)(close)  (two forward slashes, symmetric open = close)

/ RULES (both the Aria and the Python duplicate follow these exactly):
/   - (open) opens a block comment. Everything from (open) onward is dropped,
/     including the rest of that line.
/   - The next (close) closes the block. The closing (close) and everything after it
/     on that conceptual position is dropped; the byte AFTER the closing (close)
/     is the first byte processed in normal state.
/   - Content between open and close across any number of lines is dropped.
/   - (open) appearing inside a "..." string literal is NOT a delimiter.
/   - Line breaks are irrelevant — the algorithm sees a flat byte stream.
/     The block may span one line or many; it does not matter.

/ WHY "(open)(close)" AND NOT "/" :
/   Single / was the old delimiter. (open)close is visually unambiguous and
/   less likely to appear accidentally in string data.

/ BOOTSTRAP NOTE:
/   loader.py _read_re_file implements identical logic in Python.
/   If this file is ever updated, _read_re_file must be updated to match.
/   The .re file is the truth; Python is the shadow.

/ PROVIDES: COMMENT_OPEN, COMMENT_CLOSE, RA_STRIP_IN, RA_STRIP_OUT, STRIP_COMMENTS
/ DEPENDENCY: aspects.re//


NEWSET COMMENT_OPEN "//"

NEWSET COMMENT_CLOSE "//"

//STRIPPING ARIA (state machine description)

/ Entry point: STRIP_COMMENTS
/   RA_STRIP_IN  — Lux ID of first byte of NUL-terminated input byte-chain
/   RA_STRIP_OUT — Lux ID of first byte of NUL-terminated output byte-chain

/ Three states: NORMAL, BLOCK, STRING

   NORMAL + "      → STRING
/   NORMAL + (open)  → BLOCK   (drop both / bytes)
/   NORMAL + other  → NORMAL  (copy byte to output)
/   STRING + "      → NORMAL  (copy ")
/   STRING + other  → STRING  (copy byte)
   BLOCK  + (close) → NORMAL  (drop both / bytes; next byte processed in NORMAL)
   BLOCK  + other  → BLOCK   (drop byte)
/   Any    + NUL    → emit NUL, done

/ STRIP_COMMENTS is implemented below. Set RA_STRIP_IN/RA_STRIP_OUT and call STRIP_COMMENTS.//

NEW RA_STRIP_IN
NEW RA_STRIP_OUT

/── STRIP_COMMENTS: byte-stream state machine ─────────────────
/IN:  RA_STRIP_IN  = addr of first byte of NUL-terminated input chain
/     RA_STRIP_OUT = addr of first byte of NUL-terminated output chain
/OUT: output chain filled, NUL-terminated.
/States: SC_STATE = 0 (NORMAL), 1 (BLOCK), 2 (STRING)
/Uses: SC_BYTE (current byte), SC_PREV (previous byte for block-comment detection),
/      SC_IPTR (input ptr), SC_OPTR (output ptr)

NEW SC_STATE    /0=NORMAL 1=BLOCK 2=STRING
NEW SC_BYTE
NEW SC_PREV
NEW SC_IPTR
NEW SC_OPTR

NEWSET SC_ST_NORMAL 0
NEWSET SC_ST_BLOCK  1
NEWSET SC_ST_STRING 2

NOLINK
/Init
ITO STRIP_COMMENTS Move  El1=SC_ST_NORMAL Exit=SC_STATE
ITO SC_INIT_I    Move  El1=RA_STRIP_IN  Exit=SC_IPTR
ITO SC_INIT_O    Move  El1=RA_STRIP_OUT Exit=SC_OPTR
CLEAR SC_INIT_P SC_PREV
/Main loop
NOLINK
ITO CMT_LOOP      Read  El1=SC_IPTR       Exit=SC_BYTE
ITO CMT_INC       Add   El1=SC_IPTR       El2=C_1           Exit=SC_IPTR
/NUL → done
JZ SC_NULCK SC_BYTE CMT_DONE
/Dispatch by state
JEQ SC_BLKCK SC_STATE SC_ST_BLOCK SC_BLOCK_ST
JEQ SC_STRCK SC_STATE SC_ST_STRING SC_STRING_ST
/NORMAL state
/Check for '"' → STRING
JEQ SC_N_QTCK SC_BYTE DQUOTE SC_N_TO_STR
/Check for second slash after slash (block-comment open)
JEQ SC_N_SLCK SC_BYTE SLASH SC_N_SLASH
/Ordinary byte: emit, update prev
ITO SC_N_EMIT    Write El1=SC_OPTR       El2=SC_BYTE
ITO SC_N_OINC    Add   El1=SC_OPTR       El2=C_1           Exit=SC_OPTR
ITO SC_N_PREV    Move  El1=SC_BYTE       Exit=SC_PREV
ITO SC_N_JMP     Jump  Exit=CMT_LOOP
/Slash in NORMAL: check if prev was also slash → open comment
NOLINK
JEQ SC_N_SLASH SC_PREV SLASH SC_N_OPEN
/First slash: emit it, save as prev
ITO SC_N_SL1E    Write El1=SC_OPTR       El2=SC_BYTE
ITO SC_N_SL1I    Add   El1=SC_OPTR       El2=C_1           Exit=SC_OPTR
ITO SC_N_SL1P    Move  El1=SC_BYTE       Exit=SC_PREV
ITO SC_N_SL1J    Jump  Exit=CMT_LOOP
/Second slash: open comment — remove the previously emitted slash from output
NOLINK
ITO SC_N_OPEN    Sub   El1=SC_OPTR       El2=C_1           Exit=SC_OPTR
ITO SC_N_OPST    Move  El1=SC_ST_BLOCK   Exit=SC_STATE
CLEAR SC_N_OPP SC_PREV
ITO SC_N_OPJMP   Jump  Exit=CMT_LOOP
/Enter STRING state
CHAIN
    SC_N_TO_STR  Move  El1=SC_ST_STRING  Exit=SC_STATE
    SC_N_STRE    Write El1=SC_OPTR       El2=SC_BYTE
    SC_N_STRI    Add   El1=SC_OPTR       El2=C_1           Exit=SC_OPTR
    SC_N_STRP    Move  El1=SC_BYTE       Exit=SC_PREV
        ITO SC_N_STRJMP Jump Exit=CMT_LOOP
/STRING state
NOLINK
JEQ SC_STRING_ST SC_BYTE DQUOTE SC_STR_CLOSE
/Non-quote: emit
ITO SC_STR_EMIT  Write El1=SC_OPTR       El2=SC_BYTE
ITO SC_STR_INC   Add   El1=SC_OPTR       El2=C_1           Exit=SC_OPTR
ITO SC_STR_JMP   Jump  Exit=CMT_LOOP
/Closing quote → NORMAL
CHAIN
    SC_STR_CLOSE Move  El1=SC_ST_NORMAL  Exit=SC_STATE
    SC_STR_CE    Write El1=SC_OPTR       El2=SC_BYTE
    SC_STR_CI    Add   El1=SC_OPTR       El2=C_1           Exit=SC_OPTR
        ITO SC_STR_CLOSE_LB Jump  Exit=CMT_LOOP
/BLOCK state: look for closing double-slash
NOLINK
JEQ SC_BLOCK_ST SC_BYTE SLASH SC_BLK_SLASH
/Non-slash in block: drop byte
ITO SC_BLK_P     Move  El1=SC_BYTE       Exit=SC_PREV
ITO SC_BLK_JMP   Jump  Exit=CMT_LOOP
/Slash in block: check if prev was also slash → close comment
NOLINK
JEQ SC_BLK_SLASH SC_PREV SLASH SC_BLK_CLOSE
/First slash in block: save as prev, drop
ITO SC_BLK_SL1P  Move  El1=SC_BYTE       Exit=SC_PREV
ITO SC_BLK_SL1J  Jump  Exit=CMT_LOOP
/Second slash: close comment → NORMAL
NOLINK
ITO SC_BLK_CLOSE Move  El1=SC_ST_NORMAL  Exit=SC_STATE
CLEAR SC_BLK_CP SC_PREV
ITO SC_BLK_CLOSE_LB Jump  Exit=CMT_LOOP
/Done: write NUL terminator
NOLINK
ITO CMT_DONE      Write El1=SC_OPTR       El2=C_0
RREDI SC_RRET
