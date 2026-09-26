//
wax.re — the self-hosted loader (Wave 0-3, with backfill)

PUBLIC ENTRY POINT: LOAD_MAIN

This file used to be split in two: `wax.re` held the wave pipeline
(LOAD_MAIN, Wave 1-3) and `tide.re` held only Wave 0 (read a file,
strip its comments, tokenize). They were merged back into one file
under the `wax.re` name, mechanically — "tide" only ever named Wave 0,
never the whole loader; "Wax" is the loader's actual name (see
GLOSSARY.md). Grammar-level line/block parsing (LOAD_DISPATCH_LINE,
LOAD_CMD_*, indent/body reading) lives separately, in parser.re,
alongside its own PS_MAIN — wax.re calls into it per record.

Below: Wave 0, in its original words, still accurate for what it does.

WHAT WAVE 0 IS

tide (Wave 0) reads a file whole, strips its comments once, and points
the byte source at the result. Every wave after it reads clean text
and never has to know what a comment is.

WHY IT EXISTS

Python has always done this: _read_re_file reads the file,
_strip_comments cuts the comments, and the result is cached. No wave
ever sees a comment. The self-hosted loader never had that step, so it
grew live, mid-parse comment detection instead — duplicated across
BS_READ_TOKEN's whitespace skip, BS_READ_TOKEN's collection loop, and
LOAD_CMD_UNKNOWN's line skip. Three copies of one rule, each of which
got it subtly wrong at some point, while comments.re's STRIP_COMMENTS
— the canonical implementation — sat unused and uncalled.

This is the missing step. Once it runs, those three copies are dead.

HOW

  1. Read the whole file into TIDE_RAW via BS_READ_BYTE. Not a new
     reader: BS_READ_BYTE already refills 4096 at a time through
     SYS_READ, so tide just drains it into a buffer.
  2. Run STRIP_COMMENTS over TIDE_RAW into TIDE_CLEAN.
  3. Point RA_REDIRECT_BASE at TIDE_CLEAN, with RA_REDIRECT_LEN set to
     the cleaned length. BS_READ_BYTE then serves from the buffer and
     every caller above it — BS_READ_TOKEN, LOAD_DISPATCH_LINE, the
     whole dispatcher — carries on unchanged, unaware the source moved.

Step 3 is why nothing else needs to change: the redirect seam already
existed for CHAIN/FOR/SAVE bodies (registers.re), and this is the same
trick applied to a file.

BUFFERS

Two buffers, sized for the largest file in the project with room to
spare. Output is never larger than input — stripping only removes —
so both are the same size.

PROVIDES: TIDE_FILE, TIDE_RAW, TIDE_CLEAN, LOAD_MAIN
DEPENDENCY: aspects.re constants.re registers.re io.re comments.re parser.re
//

SET TIDE_BUF_SIZE Value=262144

//
SPAN, not the old BLOCK command (since removed). BLOCK named every lux
it allocated (TIDE_RAW_0, TIDE_RAW_1, ...), which at this size would have
meant half a million symbols and a symbol table eight times its proper
size. SPAN allocates the region without naming its interior — the same
choice HT_NAME_POOL_BASE and BS_HTAB_000 make, and for the same reason.
//
NEW TIDE_RAW_000  Count=262144
NEW TIDE_RAW_BASE

NEW TIDE_CLEAN_000  Count=262144
NEW TIDE_CLEAN_BASE

NEW TIDE_PTR In=OWN  /write cursor while slurping
NEW TIDE_LEN In=OWN  /cleaned length, becomes RA_REDIRECT_LEN
NEW TIDE_READY In=OWN  /1 once the base pointers have been resolved

//
TIDE_INIT: resolve buffer base addresses from their SPAN self-refs.

ONCE. Not per file — that was a real bug worth spelling out.

SPAN hands back a flat region and lets the caller own the layout inside
it; SEAL then writes the region's own address into its first lux, which
is how anything finds the base at all. But that self-ref word sits at
base+0, inside the data — so the first byte the slurp writes destroys
it. Reading it again on the next file yields that byte (47, an ASCII
slash, the first character of every file header here) instead of an
address, and everything downstream writes into low memory.

BS_INIT has the identical layout and survives because it runs exactly
once, before its buffers are ever touched. Same rule here: resolve
once, guard with TIDE_READY, and never read the self-ref again.
//
NOLINK
JZ TIDE_INIT El1=TIDE_READY Exit=TIDE_INIT_DO
RREDI TIDE_INIT_SKIP
NOLINK
NOVA TIDE_INIT_DO Op=Move El1=TIDE_RAW_000 Exit=TIDE_RAW_BASE
NOVA Op=Move El1=TIDE_CLEAN_000 Exit=TIDE_CLEAN_BASE
NOVA Op=Move El1=C_1 Exit=TIDE_READY

RREDI TIDE_INIT_RET

//
TIDE_SLURP: drain the open file into TIDE_RAW, NUL-terminated.

IN:  RA_LOAD_FD open, redirect OFF (so BS_READ_BYTE reads the file)
OUT: TIDE_RAW filled, TIDE_PTR = one past the last byte

Uses BS_READ_BYTE rather than its own SYS_READ loop: that function
already handles buffer refills, EOF and the redirect seam. Duplicating
it here to read "more efficiently" would be a second reader to keep
correct, for no gain — the syscalls are identical either way.
//
NOLINK
NOVA TIDE_SLURP Op=Move El1=TIDE_RAW_BASE Exit=TIDE_PTR
RVOCA TIDE_SL_RD Body=BS_READ_BYTE
JZ TIDE_SL_EOFCK El1=RA_LOAD_BYTE Exit=TIDE_SL_DONE
NOVA TIDE_SL_W Op=Write El1=TIDE_PTR El2=RA_LOAD_BYTE
NOVA TIDE_SL_INC Op=Add El1=TIDE_PTR El2=C_1 Exit=TIDE_PTR
NOVA TIDE_SL_JMP Op=Jump Exit=TIDE_SL_RD
NOLINK
NOVA TIDE_SL_DONE Op=Write El1=TIDE_PTR El2=C_0
RREDI TIDE_SL_RET

//
TIDE_FILE: the whole pass, for one already-open file.

IN:  RA_LOAD_FD open and positioned at 0, redirect OFF
OUT: redirect ON, pointing at the cleaned text

After this returns, LOAD_DISPATCH_LINE reads comment-free bytes.
LOAD_FILE turns the redirect back off when it closes the file, so the
next file's slurp reads from disk again — that ordering matters, and
is the one thing here that will bite if it is ever changed.
//
NOLINK
RVOCA TIDE_FILE Body=TIDE_INIT
RVOCA TIDE_F_SLURP Body=TIDE_SLURP
/Strip: TIDE_RAW → TIDE_CLEAN
NOVA TIDE_F_IN Op=Move El1=TIDE_RAW_BASE Exit=RA_STRIP_IN
NOVA TIDE_F_OUT Op=Move El1=TIDE_CLEAN_BASE Exit=RA_STRIP_OUT
RVOCA TIDE_F_STRIP Body=STRIP_COMMENTS
/Cleaned length = SL_OPTR - TIDE_CLEAN_BASE (SL_OPTR is left one past
/the last byte written, before the NUL terminator).
NOVA TIDE_F_LEN Op=Sub El1=SL_OPTR El2=TIDE_CLEAN_BASE Exit=TIDE_LEN
/Point the byte source at the cleaned buffer
NOVA TIDE_F_RB Op=Move El1=TIDE_CLEAN_BASE Exit=RA_REDIRECT_BASE
NOVA TIDE_F_RL Op=Move El1=TIDE_LEN Exit=RA_REDIRECT_LEN
CLEAR TIDE_F_RP El1=RA_REDIRECT_POS
RREDI TIDE_F_RET

NEW TIDE_RECORDS_HEAD In=OWN
NEW TIDE_REC_TAIL In=OWN
NEW TIDE_CUR_TOK_TAIL In=OWN
NEW TIDE_TMP In=OWN
NEW TIDE_TMP2 In=OWN
NEW TIDE_TMP3 In=OWN
NEW TIDE_TMP4 In=OWN
NEW TIDE_TOK In=OWN
NEW TIDE_LASTPOS In=OWN  /RA_REDIRECT_POS just before each BS_READ_TOKEN call
NEW TIDE_SCANPOS In=OWN  /byte-scan cursor, looking for a skipped LF
NEW TIDE_SCANEND In=OWN  /scan stops here: the new token's own start position
SET TIDE_REC_SIZE Value=3
SET TIDE_TOK_SIZE Value=4

//
── TIDE_RESTRUCTURE state ────────────────────────────────────────────
The indent/body-collection pass (TIDE_RESTRUCTURE, below) that runs
inside LOAD_TOKENIZE_FILE right after TIDE_BUILD_RECORDS -- the Reca
half of Python's _parse_indent_blocks, called from _read_re_file the
same way (a subroutine of the file-read pipeline, NOT a separate wave).

Record field 2 is dual-purpose across the two phases, exactly as
Python's line record is: TIDE_BUILD_RECORDS writes each record's own
indent (leading-space count) there; TIDE_RESTRUCTURE reads that indent
to decide nesting, then overwrites field 2 -- with the head of a
collected body chain for a block opener, or 0 for every other record.
So downstream (LOAD_PREPASS_FILE / LOAD_FILE) sees field 2 as "body
chain, 0 if none", never as an indent.

Context stack: one 3-word entry per open block --
  [opener_indent, opener_record_addr, body_tail_addr]
opener_indent  closes the block when a later record's indent is <= it.
opener_record  is where field 2 (its body head) gets written.
body_tail      is the last record appended to that block's body, for
               O(1) append (its field 1 is repointed to the next body
               record). 32 levels * 3 = 96; nesting is shallow (a FOR
               with a SWITCH inside is 2), same headroom convention as
               SK_PLINK_BUF and friends -- CTX_MAX_DEPTH is 8.
//
NEW TIDE_RST_STACK  Count=96
NEW TIDE_RST_SBASE  /base addr of TIDE_RST_STACK (set at init, like SK_IND_STACK_BASE)
NEW TIDE_RST_SP In=OWN    /stack pointer: word offset into the stack, 0 = empty
NEW TIDE_RST_CUR In=OWN   /record currently being placed
NEW TIDE_RST_NEXT In=OWN  /its successor on the original chain, saved before relinking
NEW TIDE_RST_PREV In=OWN  /last record kept on the main chain (for its field-1 relink)
NEW TIDE_RST_HEAD In=OWN  /new main-chain head (openers + top-level records only)
NEW TIDE_RST_IND In=OWN   /current record's own indent, read from field 2
/Opener recognition is table-driven, not a hand-kept list: TR_IS_OP walks
/SK_OPENER_TABLE (declared with the other LOAD_MAIN tables, filled from
/the corpus's Open= declarations). See there.

//
TIDE_BUILD_RECORDS: tokenize the clean, redirected buffer into a chain.

IN:  redirect already ON, pointing at cleaned text (TIDE_FILE's own job,
     must run first).
OUT: TIDE_RECORDS_HEAD = first record (0 if the file produced none).

Record: (first_token, next_record, indent) -- TIDE_REC_SIZE luces.
        Field 2 holds this line's indent here; TIDE_RESTRUCTURE later
        repurposes it as the record's body-chain head (see its own
        header, and the TIDE_RESTRUCTURE state block above).
Token:  (packed_addr, length, hash, next_token) -- TIDE_TOK_SIZE luces.
Both are ordinary Lux chains -- the same Next-pointer idiom as
everything else in this project, not a sized array.

Line-boundary detection: BS_READ_TOKEN's own collection loop consumes
the LF while checking a token's own boundary (peeking the byte after
the token ends advances the read position past it, with no pushback) --
so by the time BS_READ_TOKEN reports "no token here" for the NEXT read,
the LF that ended the PREVIOUS line is already gone, never observable
as RA_LOAD_BYTE the way true EOF (byte==0) is. A single LF between two
non-blank lines is therefore invisible to the old "tlen==0, check
RA_LOAD_BYTE" approach entirely.

Fix: independently scan the source buffer for any LF skipped between
the previous read position (TIDE_LASTPOS) and where the new token
actually starts (RA_REDIRECT_POS - RA_LOAD_TLEN) -- this doesn't depend
on BS_READ_TOKEN's own byte-tracking at all.
//
NOLINK
CLEAR TIDE_BUILD_RECORDS El1=TIDE_RECORDS_HEAD
CLEAR TBR_TAILCLR El1=TIDE_REC_TAIL
CLEAR TBR_LPCLR El1=TIDE_LASTPOS
RVOCA TBR_START Body=TBR_NEWREC
NOVA TBR_JMP2RT0 Op=Jump Exit=TBR_RT_TOK

NOLINK
── TBR_NEWREC: allocate a record, link it onto the chain, reset the
── current record's own token-tail. Callable (RVOCA/RREDI): used both
── at the very start and whenever a line boundary is detected mid-stream.
NOVA TBR_NEWREC Op=Move El1=C_0 Exit=TIDE_CUR_TOK_TAIL

ALLOC_TO TBR_ALLOCREC Exit=TIDE_TMP Count=TIDE_REC_SIZE
NOVA TBR_R_FT0 Op=Write El1=TIDE_TMP El2=C_0
NOVA TBR_R_ADDN Op=Add El1=TIDE_TMP El2=C_1 Exit=TIDE_TMP2
NOVA TBR_R_NX0 Op=Write El1=TIDE_TMP2 El2=C_0
/Field 2 = indent, defaulted to 0 here (a record whose first token sits
/at column 0, or one that never gets an indent computed, reads as 0).
/TBR_CALC_INDENT overwrites it with the real leading-space count when a
/line boundary opens this record. TIDE_TMP still holds the record base.
NOVA TBR_R_ADDI Op=Add El1=TIDE_TMP El2=C_2 Exit=TIDE_TMP2
NOVA TBR_R_IN0 Op=Write El1=TIDE_TMP2 El2=C_0
/Attach via LIST_APPEND (alloc.re). Head is a register here, so the
/head reference is that register's own address.
NOVA TBR_R_LLN Op=Move El1=TIDE_TMP Exit=RA_LL_NODE
NOVA TBR_R_LLO Op=Move El1=C_1 Exit=RA_LL_NEXTOFF
NOVA TBR_R_LLT Op=Idem El1=TIDE_REC_TAIL Exit=RA_LL_TAIL_REF
NOVA TBR_R_LLH Op=Idem El1=TIDE_RECORDS_HEAD Exit=RA_LL_HEAD_REF
RVOCA TBR_R_LLAPP Body=LIST_APPEND
RREDI TBR_NEWREC_RET

NOLINK
── TBR_RT_TOK: read the next token, detect any crossed line boundary.
── TIDE_LASTPOS holds the PREVIOUS token's own boundary-byte position
── (set at the end of TBR_STORE_TOK, not freshly here -- by the time
── control reaches here, that boundary byte is already behind
── RA_REDIRECT_POS, consumed by the previous BS_READ_TOKEN call itself).
RVOCA TBR_RT_TOK Body=BS_READ_TOKEN
JZ TBR_TLENCK El1=RA_LOAD_TLEN Exit=TBR_MAYBE_EOF
/A real token was found. Where did it actually start? BS_READ_TOKEN's
/own boundary-check consumes one extra byte past the token's real
/content (the space/LF that ended it), so RA_REDIRECT_POS overcounts
/by exactly one relative to the token's own last real character.
NOVA TBR_SAVELEN Op=Move El1=RA_LOAD_TLEN Exit=TIDE_TMP4
NOVA TBR_TSTART1 Op=Sub El1=RA_REDIRECT_POS El2=TIDE_TMP4 Exit=TIDE_SCANEND
NOVA TBR_TSTART2 Op=Sub El1=TIDE_SCANEND El2=C_1 Exit=TIDE_SCANEND
NOVA TBR_SCANP0 Op=Move El1=TIDE_LASTPOS Exit=TIDE_SCANPOS
JEQ TBR_SCANLOOP El1=TIDE_SCANPOS El2=TIDE_SCANEND Exit=TBR_NOLF
NOVA TBR_SCANADDR Op=Add El1=TIDE_CLEAN_BASE El2=TIDE_SCANPOS Exit=TIDE_TMP2
NOVA TBR_SCANREAD Op=Read El1=TIDE_TMP2 Exit=TIDE_TMP3
JEQ TBR_SCANLFCK El1=TIDE_TMP3 El2=LF Exit=TBR_FOUNDLF
NOVA TBR_SCANINC Op=Add El1=TIDE_SCANPOS El2=C_1 Exit=TIDE_SCANPOS
NOVA TBR_SCANJMP Op=Jump Exit=TBR_SCANLOOP

NOLINK
── TBR_FOUNDLF: a line boundary was crossed -- close current record,
── open a new one, compute that new record's indent (the scan positions
── TIDE_SCANPOS/TIDE_SCANEND are still live here), THEN store the token
── already held (not re-read).
RVOCA TBR_FOUNDLF Body=TBR_NEWREC
RVOCA TBR_FOUNDLF_CI Body=TBR_CALC_INDENT
NOVA TBR_NR1_JMP Op=Jump Exit=TBR_STORE_TOK

NOLINK
── TBR_NOLF: the byte-scan found no LF between the previous token and
── this one -- so this token is on the SAME line as the record already
── open; store it directly. One exception: the file's very first token
── reaches here too (TIDE_LASTPOS started at 0, its own scan finds no
── LF), and its record's indent was never computed at a line boundary.
── So when this is the current record's first token
── (TIDE_CUR_TOK_TAIL == 0) compute the indent first; every later
── same-line token has TIDE_CUR_TOK_TAIL != 0 and the indent already set.
JZ TBR_NOLF El1=TIDE_CUR_TOK_TAIL Exit=TBR_NOLF_CI
NOVA TBR_NOLF_JMP Op=Jump Exit=TBR_STORE_TOK
NOLINK
RVOCA TBR_NOLF_CI Body=TBR_CALC_INDENT
NOVA TBR_NOLF_CIJMP Op=Jump Exit=TBR_STORE_TOK

NOLINK
── TBR_CALC_INDENT: compute the current record's indent = count of
── leading spaces on its line, stored into the record's field 2
── (TIDE_REC_TAIL + 2). TIDE_SCANEND points AT this token's own first
── byte (= token start; the same value the LF-scan in TBR_RT_TOK uses as
── its exclusive upper bound). The leading spaces are therefore the bytes
── immediately BEFORE it, so the scan starts one byte back (SCANEND - 1)
── and walks backwards over consecutive SP bytes, stopping at the
── preceding LF, any non-space, or the buffer start (pos < 0). Starting
── AT SCANEND instead read the token's own first (non-space) char and
── stopped immediately -- every line's indent came back 0, so no block
── body ever nested (TIDE_RESTRUCTURE saw every record at indent 0) and
── the self-hosted load wave found empty SWITCH/FOR/... bodies. Blank
── lines collapse naturally: the scan stops at the nearest LF, so the
── count is spaces after the last line break -- the same number Python's
── _count_indent returns (it counts ' ' as 1; TAB, worth 4 there, never
── occurs as .re indentation in this corpus). Callable (RVOCA/RREDI).
── Scratch: TIDE_TMP (count), TIDE_TMP2 (pos then field addr), TIDE_TMP3
── (byte). Must NOT touch TIDE_TMP4 -- it still holds the held token's
── length for TBR_STORE_TOK.
NOVA TBR_CALC_INDENT Op=Move El1=C_0 Exit=TIDE_TMP
NOVA TBR_CI_POS Op=Sub El1=TIDE_SCANEND El2=C_1 Exit=TIDE_TMP2
NOVA TBR_CI_LOOP Op=Less El1=TIDE_TMP2 El2=C_0 Exit=RA_FLAG
JZ TBR_CI_NEGCK El1=RA_FLAG Exit=TBR_CI_READ
NOVA TBR_CI_NEGJ Op=Jump Exit=TBR_CI_DONE
NOLINK
NOVA TBR_CI_READ Op=Add El1=TIDE_CLEAN_BASE El2=TIDE_TMP2 Exit=TIDE_TMP3
NOVA TBR_CI_RD2 Op=Read El1=TIDE_TMP3 Exit=TIDE_TMP3
JEQ TBR_CI_SPCK El1=TIDE_TMP3 El2=SP Exit=TBR_CI_SP
NOVA TBR_CI_NONSP Op=Jump Exit=TBR_CI_DONE
NOLINK
NOVA TBR_CI_SP Op=Add El1=TIDE_TMP El2=C_1 Exit=TIDE_TMP
NOVA TBR_CI_DEC Op=Sub El1=TIDE_TMP2 El2=C_1 Exit=TIDE_TMP2
NOVA TBR_CI_LJMP Op=Jump Exit=TBR_CI_LOOP
NOLINK
NOVA TBR_CI_DONE Op=Add El1=TIDE_REC_TAIL El2=C_2 Exit=TIDE_TMP2
NOVA TBR_CI_STORE Op=Write El1=TIDE_TMP2 El2=TIDE_TMP
RREDI TBR_CI_RET

NOLINK
── TBR_STORE_TOK: store the token currently held (BS_TOKBUF_BASE/
── RA_LOAD_TLEN/RA_LOAD_HASH already set by BS_READ_TOKEN) into
── whichever record is current (TIDE_REC_TAIL) -- reached either
── directly (same line as before) or after TBR_NEWREC (new line).
── Before doing anything else, save this token's own boundary-byte
── position (RA_REDIRECT_POS - 1) as TIDE_LASTPOS, for the NEXT read's
── scan to start from -- must happen here, not in TBR_RT_TOK, since by
── then the boundary byte is already behind RA_REDIRECT_POS.
RVOCA TBR_STORE_TOK Body=BS_PACK_TOKBUF
NOVA TBR_SAVEBOUND1 Op=Sub El1=RA_REDIRECT_POS El2=C_1 Exit=TIDE_LASTPOS
ALLOC_TO TBR_T_ALLOC Exit=TIDE_TOK Count=TIDE_TOK_SIZE
NOVA TBR_T_ADDR Op=Write El1=TIDE_TOK El2=RA_BS_TMP2
NOVA TBR_T_LADDR Op=Add El1=TIDE_TOK El2=C_1 Exit=TIDE_TMP2
NOVA TBR_T_LEN Op=Write El1=TIDE_TMP2 El2=TIDE_TMP4
NOVA TBR_T_HADDR Op=Add El1=TIDE_TOK El2=C_2 Exit=TIDE_TMP2
NOVA TBR_T_HASH Op=Write El1=TIDE_TMP2 El2=RA_LOAD_HASH
NOVA TBR_T_NADDR Op=Add El1=TIDE_TOK El2=C_3 Exit=TIDE_TMP2
NOVA TBR_T_NX0 Op=Write El1=TIDE_TMP2 El2=C_0
/Attach via LIST_APPEND (alloc.re). The head of a record's token list is
/field 0 of the record itself, so the head reference is the record's own
/address -- the same routine, a different reference.
NOVA TBR_T_LLN Op=Move El1=TIDE_TOK Exit=RA_LL_NODE
NOVA TBR_T_LLO Op=Move El1=C_3 Exit=RA_LL_NEXTOFF
NOVA TBR_T_LLT Op=Idem El1=TIDE_CUR_TOK_TAIL Exit=RA_LL_TAIL_REF
NOVA TBR_T_LLH Op=Move El1=TIDE_REC_TAIL Exit=RA_LL_HEAD_REF
RVOCA TBR_T_LLAPP Body=LIST_APPEND
/The tail update, the empty-list branch and the jump that used to tie
/them together all live in LIST_APPEND now.
NOVA TBR_T_JMP2RD Op=Jump Exit=TBR_RT_TOK

NOLINK
── TBR_MAYBE_EOF: no token found -- true EOF, or just more whitespace ──
NOVA TBR_MAYBE_EOF Op=Move El1=RA_LOAD_BYTE Exit=TIDE_TMP

JZ TBR_EOFCK El1=TIDE_TMP Exit=TBR_DONE
NOVA TBR_JMP2RT Op=Jump Exit=TBR_RT_TOK

NOLINK
RREDI TBR_DONE


//
TIDE_RESTRUCTURE: turn the flat record chain TIDE_BUILD_RECORDS just
built into an indent-nested one, in a single forward pass. The Reca half
of Python's _parse_indent_blocks (loader.py) -- called from
LOAD_TOKENIZE_FILE exactly where Python calls it from _read_re_file: a
step of the per-file read pipeline, NOT a separate wave.

IN:  TIDE_RECORDS_HEAD = flat chain head; each record's field 2 holds its
     own indent (leading-space count), written by TIDE_BUILD_RECORDS.
OUT: TIDE_RECORDS_HEAD = head of the restructured MAIN chain -- only
     top-level records and block openers remain on it. Every block
     opener (FOR/SOMA/SWITCH/MATCH/SAVE, matched by first-token hash)
     has its body records moved onto a private chain hung off its own
     field 2 (0 when the block has no body). Body records leave the main
     chain entirely; nesting works because a nested opener, itself a body
     record of its parent, keeps its OWN body on its OWN field 2 while
     its field 1 threads it into the parent's body chain -- two
     independent links, one per field.

Closing rule, identical to Python's (a level ends at the first later line
whose indent is <= the opener's): pop every context whose opener_indent
is >= the current record's indent before placing that record. What
remains open (if anything) encloses it.

Scope note -- deferred, on purpose: Python also splits each body by
sub-level here (FOR override lines vs body lines, SWITCH case fields,
MATCH leaves) and drops stray/garbage sub-indents ('skip'). This pass
does the STRUCTURAL half only: every deeper record is attached to the
enclosing opener's body chain verbatim, in order. The per-mode
interpretation of that body (rel>0 overrides, case parsing, ...) belongs
to the Wave-1 sizer and Wave-2 build handlers that read the body chain,
which is where Python defers the codegen too (__body__/__slots__/
__switch__ records). Finer level-classification lands with those handlers.

Context stack entry (3 words, TIDE_RST_STACK, SP = word offset, 0=empty):
  [SP-3] opener_indent   [SP-2] opener_record   [SP-1] body_tail (0=empty)
//
NOLINK
CLEAR TIDE_RESTRUCTURE El1=TIDE_RST_SP
CLEAR TR_RS_PREV0 El1=TIDE_RST_PREV
CLEAR TR_RS_HEAD0 El1=TIDE_RST_HEAD
NOVA TR_RS_CUR0 Op=Move El1=TIDE_RECORDS_HEAD Exit=TIDE_RST_CUR

/TR_LOOP: place one record. Loop head -- reached by fall-through the
/first time (this header is a /-comment, stripped, so the Move just above
/auto-links straight onto this JZ), by TR_ADVANCE thereafter.
JZ TR_LOOP El1=TIDE_RST_CUR Exit=TR_DONE
/Save the successor before any relinking rewrites cur's field 1.
NOVA TR_L_NXA Op=Add El1=TIDE_RST_CUR El2=C_1 Exit=TIDE_TMP
NOVA TR_L_NX Op=Read El1=TIDE_TMP Exit=TIDE_RST_NEXT
/Read this record's indent (field 2) into TIDE_RST_IND, then clear field
/2 -- it becomes a body-chain head for an opener, 0 for everything else.
NOVA TR_L_INDA Op=Add El1=TIDE_RST_CUR El2=C_2 Exit=TIDE_TMP
NOVA TR_L_IND Op=Read El1=TIDE_TMP Exit=TIDE_RST_IND
NOVA TR_L_RST Op=Write El1=TIDE_TMP El2=C_0

/TR_POP: close every context whose opener_indent >= cur indent. Loop head
/-- fall-through first (/-comment header, stripped, so TR_L_RST auto-links
/onto this JZ), TR_P_JMP thereafter.
JZ TR_POP El1=TIDE_RST_SP Exit=TR_POPDONE
NOVA TR_P_OA Op=Sub El1=TIDE_RST_SP El2=C_3 Exit=TIDE_TMP2
NOVA TR_P_BASE Op=Add El1=TIDE_RST_SBASE El2=TIDE_TMP2 Exit=TIDE_TMP3
NOVA TR_P_RD Op=Read El1=TIDE_TMP3 Exit=TIDE_TMP
/flag = (opener_indent < cur_indent). If so, cur is deeper -> the block
/stays open, stop popping. Else opener_indent >= cur_indent -> pop.
NOVA TR_P_LESS Op=Less El1=TIDE_TMP El2=TIDE_RST_IND Exit=RA_FLAG
NOVA TR_P_JIF Op=JumpIf El1=RA_FLAG Exit=TR_POPDONE
NOVA TR_P_DEC Op=Sub El1=TIDE_RST_SP El2=C_3 Exit=TIDE_RST_SP
NOVA TR_P_JMP Op=Jump Exit=TR_POP

NOLINK
── TR_POPDONE: stack now reflects cur's enclosure. Empty -> cur is a
── main-chain record; non-empty -> cur is a body record of the top.
JZ TR_POPDONE El1=TIDE_RST_SP Exit=TR_MAIN
/── Body record: append cur to the top opener's body chain. ──
NOVA TR_B_O Op=Sub El1=TIDE_RST_SP El2=C_3 Exit=TIDE_TMP
NOVA TR_B_BASE Op=Add El1=TIDE_RST_SBASE El2=TIDE_TMP Exit=TIDE_TMP
NOVA TR_B_TAILA Op=Add El1=TIDE_TMP El2=C_2 Exit=TIDE_TMP2
NOVA TR_B_TAIL Op=Read El1=TIDE_TMP2 Exit=TIDE_TMP3
JZ TR_B_FIRSTCK El1=TIDE_TMP3 Exit=TR_B_FIRST
/Non-empty body: body_tail.field1 = cur.
NOVA TR_B_LINKA Op=Add El1=TIDE_TMP3 El2=C_1 Exit=TIDE_TMP4
NOVA TR_B_LINK Op=Write El1=TIDE_TMP4 El2=TIDE_RST_CUR
NOVA TR_B_LJ Op=Jump Exit=TR_B_SETTAIL
NOLINK
/First body: opener.field2 = cur (opener_record = stack[SP-2]).
NOVA TR_B_FIRST Op=Add El1=TIDE_TMP El2=C_1 Exit=TIDE_TMP4
NOVA TR_B_OPR Op=Read El1=TIDE_TMP4 Exit=TIDE_TMP4
NOVA TR_B_OPF2 Op=Add El1=TIDE_TMP4 El2=C_2 Exit=TIDE_TMP4
NOVA TR_B_OPW Op=Write El1=TIDE_TMP4 El2=TIDE_RST_CUR
/Shared tail of both branches -- the first-body branch above falls straight
/in here (no NOLINK), the non-empty branch jumps here (TR_B_LJ).
/top.body_tail = cur; terminate cur.field1 (a later sibling overwrites it).
NOVA TR_B_SETTAIL Op=Write El1=TIDE_TMP2 El2=TIDE_RST_CUR
NOVA TR_B_CN Op=Add El1=TIDE_RST_CUR El2=C_1 Exit=TIDE_TMP3
NOVA TR_B_CNW Op=Write El1=TIDE_TMP3 El2=C_0
NOVA TR_B_JMP Op=Jump Exit=TR_OPENERCK

NOLINK
── TR_MAIN: cur stays on the main chain. Link prev.field1 = cur, or set
── the new head if cur is the first main record.
JZ TR_MAIN El1=TIDE_RST_HEAD Exit=TR_M_SETHEAD
NOVA TR_M_PLA Op=Add El1=TIDE_RST_PREV El2=C_1 Exit=TIDE_TMP2
NOVA TR_M_PLW Op=Write El1=TIDE_TMP2 El2=TIDE_RST_CUR
NOVA TR_M_PJ Op=Jump Exit=TR_M_SETPREV
NOLINK
NOVA TR_M_SETHEAD Op=Move El1=TIDE_RST_CUR Exit=TIDE_RST_HEAD
/Set-head branch falls straight in here; the prev-link branch jumps here
/(TR_M_PJ). Shared tail: record cur as the new prev, terminate its field 1.
NOVA TR_M_SETPREV Op=Move El1=TIDE_RST_CUR Exit=TIDE_RST_PREV
NOVA TR_M_CN Op=Add El1=TIDE_RST_CUR El2=C_1 Exit=TIDE_TMP2
NOVA TR_M_CNW Op=Write El1=TIDE_TMP2 El2=C_0
NOVA TR_M_OJ Op=Jump Exit=TR_OPENERCK

NOLINK
── TR_OPENERCK: is cur itself an opener? If so push it, so its own
── (deeper) body records attach to it. Applies whether cur landed on the
── main chain or in a parent's body -- a nested opener needs both.
RVOCA TR_OPENERCK Body=TR_IS_OP
JZ TR_O_JZ El1=TIDE_TMP Exit=TR_ADVANCE
/push: stack[SP]=indent, stack[SP+1]=cur, stack[SP+2]=0; SP += 3
NOVA TR_O_PA Op=Add El1=TIDE_RST_SBASE El2=TIDE_RST_SP Exit=TIDE_TMP2
NOVA TR_O_PW Op=Write El1=TIDE_TMP2 El2=TIDE_RST_IND
NOVA TR_O_P1A Op=Add El1=TIDE_TMP2 El2=C_1 Exit=TIDE_TMP3
NOVA TR_O_P1W Op=Write El1=TIDE_TMP3 El2=TIDE_RST_CUR
NOVA TR_O_P2A Op=Add El1=TIDE_TMP2 El2=C_2 Exit=TIDE_TMP3
NOVA TR_O_P2W Op=Write El1=TIDE_TMP3 El2=C_0
NOVA TR_O_SP Op=Add El1=TIDE_RST_SP El2=C_3 Exit=TIDE_RST_SP
/TR_ADVANCE: next record. The push above falls straight in here
/(/-comment header, stripped, no NOLINK); the not-opener case jumps here
/(TR_O_JZ).
NOVA TR_ADVANCE Op=Move El1=TIDE_RST_NEXT Exit=TIDE_RST_CUR
NOVA TR_A_JMP Op=Jump Exit=TR_LOOP

NOLINK
── TR_DONE: publish the restructured main head.
NOVA TR_DONE Op=Move El1=TIDE_RST_HEAD Exit=TIDE_RECORDS_HEAD
RREDI TR_RET

//
── TR_IS_OP: does cur's first token name a block opener? ──────────────
IN:  TIDE_RST_CUR = record.
OUT: TIDE_TMP = 1 if the first token's hash is in SK_OPENER_TABLE, else 0.
Walks the 0-terminated opener table (filled from the corpus's Open=
declarations, see SK_OPENER_TABLE) comparing the record's first-token
hash to each entry -- adaptive, no hand-kept opener list here. Scratch:
TIDE_TMP2 (first token then table cursor), TIDE_TMP3 (token hash),
TIDE_TMP4 (table entry). Leaf but for its own RREDI. An empty record (no
first token) is not an opener.
//
NOLINK
CLEAR TR_IS_OP El1=TIDE_TMP
NOVA TR_IO_FT Op=Read El1=TIDE_RST_CUR Exit=TIDE_TMP2
JZ TR_IO_FTZ El1=TIDE_TMP2 Exit=TR_IO_RET
NOVA TR_IO_HA Op=Add El1=TIDE_TMP2 El2=C_2 Exit=TIDE_TMP3
NOVA TR_IO_H Op=Read El1=TIDE_TMP3 Exit=TIDE_TMP3
NOVA TR_IO_P0 Op=Move El1=SK_OPENER_TABLE_BASE Exit=TIDE_TMP2
NOVA TR_IO_LOOP Op=Read El1=TIDE_TMP2 Exit=TIDE_TMP4
JZ TR_IO_ENDCK El1=TIDE_TMP4 Exit=TR_IO_RET
JEQ TR_IO_MCK El1=TIDE_TMP4 El2=TIDE_TMP3 Exit=TR_IO_YES
NOVA TR_IO_INC Op=Add El1=TIDE_TMP2 El2=C_1 Exit=TIDE_TMP2
NOVA TR_IO_LJMP Op=Jump Exit=TR_IO_LOOP
NOLINK
NOVA TR_IO_YES Op=Move El1=C_1 Exit=TIDE_TMP
RREDI TR_IO_RET


//
────────────────────────────────────────────────────────────────
Below: Wave 1-3 (lcount / declare / load), moved here from wax.re.
Mechanical move, not a rewrite -- see HISTORY.md for the date/reason.
tide now owns the whole wave pipeline (0 through 3), not just the
file-read + comment-strip + tokenize step (Wave 0) it started as.
Grammar-level line parsing (recognizing NOVA/NEW/SET/LINK, reading
El1=/El2=/Exit=, block/indent handling) lives in parser.re -- tide
calls into it per record, the same way it already calls STRIP_COMMENTS
in comments.re.
────────────────────────────────────────────────────────────────
//
//
── LOAD_MAIN interface — input parameters ────────────────────
Written by caller (loader.py or future Reca loader) before calling LOAD_MAIN.
Convention matches SC_A0/SC_A1 style: caller places args, callee reads them.
//
NEW BS_FILE_LIST   /addr of file path array (packed strings, stride=1)
NEW BS_FILE_COUNT  /number of files to load


── Wax internal state (wave/file bookkeeping) ─────────────────
/File iteration
NEW SK_FIDX           /current file index
NEW SK_FPATH          /current file path addr (packed string)
/Declare-before-use barrier (Pattern Z, BUGS.md): 1 during the
/declare-only pass (every file, declaration-capable commands only --
/see LOAD_CMD_UNKNOWN's own check), 0 during Wave 1 (today's existing
/single-pass-per-file, now safe since every name already exists).
NEW SK_DECLARE_ONLY
/Backfill buffer: (slot_addr, name_hash) pairs, 0-terminated
NEW SK_BACKFILL_BUF  Count=32768
NEW SK_BACKFILL_BASE
NEW SK_BACKFILL_PTR
//
Lcount table: [addr, count, addr, count, ..., 0] — max ~1024 distinct src names.
Stride 2 per entry (hash, count). Lux-kind offset for ADD_LUMEN is now
determined by NOVA_ADDRS_CHECK (Python parity: _nova_addrs_set), not stored
in the table.
//
NEW SK_LCOUNT_BUF  Count=2048
NEW SK_LCOUNT_BASE
NEW SK_LCOUNT_PTR
//
NOVA address registry: linear array of all NOVA lux addresses, 0-terminated.
Populated by LOAD_ALLOC_SIZED when RA_ALLOC_BASE==NOVA_SIZE (Python parity:
_alloc_nova → _nova_addrs_set.add). Checked by NOVA_ADDRS_CHECK to
determine RA_LM_OFFSET (NOVA_SIZE or C_1) for ADD_LUMEN / REMOVE_LUMEN.
//
NEW SK_NOVA_ADDRS_BUF  Count=2048
NEW SK_NOVA_ADDRS_BASE
NEW SK_NOVA_ADDRS_PTR
//
Pending-LINK buffer: (src_symtab_addr, rel_symtab_addr, tgt_symtab_addr) triples,
0-terminated. Recorded by parser.re's PS_CMD_LINK during Wave 1 (symtab addrs
may not yet be resolved — forward refs). Applied by LOAD_APPLY_LINKS after Wave 1, once
every name's address and lux-kind prefix are known.
//
NEW SK_PLINK_BUF  Count=2048
NEW SK_PLINK_BASE
NEW SK_PLINK_PTR   /write cursor — used while recording in parser.re's PS_CMD_LINK/
NEW SK_PLINK_ITER  /read cursor — used while applying in LOAD_APPLY_LINKS/
/Declare-only allowlist (see LOAD_CMD_UNKNOWN): one command address per
/slot, 0-terminated. Filled at init, because a command's own address is
/only obtainable via Idem at runtime. Adding a command to the pass is
/one line in LOAD_MAIN's fill sequence -- previously it meant inserting
/a fetch/compare pair into an eleven-deep chain of jumps.
NEW SK_DECL_TABLE  Count=16
NEW SK_DECL_TABLE_BASE /base addr of SK_DECL_TABLE (SPAN does not self-ref)
NEW SK_DECL_PTR        /walk pointer used by the allowlist loop
/Block-opener hashes (TIDE_RESTRUCTURE reads this): one full-64-bit djb2
/per opener command word, 0-terminated. Filled at init by Python
/(prepare_reca_freeze) straight from the corpus's own Cmd=WORD Open=mode
/declarations -- the same single source Python's _discover_block_openers
/reads -- so adding a new block opener is one Open= line in commands.re,
/no hand-kept list anywhere. Hashes (not addresses) because the consumer
/matches a record's first-token hash, which is what tide already stored.
NEW SK_OPENER_TABLE  Count=32
NEW SK_OPENER_TABLE_BASE /base addr of SK_OPENER_TABLE (Idem-derived, like SK_DECL_TABLE_BASE)
/Region combo table (B): [name-hash, combo-bitmask] pairs, 0-terminated.
/Filled at init by Python (prepare_reca_freeze) from _name_regions -- the
/one place region membership (In=/K_/RA_) is derived, on the source-of-truth
/side. REGION_LOOKUP (alloc.re) walks it by name hash. Same pre-baked-table
/pattern as SK_DECL_TABLE/SK_OPENER_TABLE. ~347 region names today; 1024
/pairs of headroom.
NEW SK_REGION_TABLE  Count=2048
NEW SK_REGION_TABLE_BASE /base addr of SK_REGION_TABLE (Idem-derived, like the others)
/SK_DECL_FILL removed: table is pre-populated by Python (prepare_reca_freeze)
/Bootstrap-time name buffer: HT_INSERT (htable.re, this session) now
/always needs RA_HT_NAME/RA_HT_NLEN (the name's own packed bytes, for
/full-name comparison on every hash match -- see BUGS.md Pattern Z).
/During early bootstrap (SEAL/word-registration below), the command
/word being inserted only ever existed transiently, byte-by-byte, as
/part of computing its own djb2 hash -- never as a contiguous buffer.
/10 slots is enough for the longest bootstrap word ("ALLOC_TO", 8
/characters); each ASCII byte gets its own aether slot, matching
/BS_TOKBUF_BASE's own storage convention (confirmed against
/BS_READ_TOKEN earlier this session).
NEW SK_BOOT_NAMEBUF  Count=50
NEW SK_BOOT_NAMEBUF_ADDR In=OWN  /SK_BOOT_NAMEBUF's own real address, captured
                                  /once via Idem (SPAN regions don't self-ref)
                                  /and reused by every SEAL/WORD_BOOT site that
                                  /needs it -- see BUGS.md for the bug this fixes.
NEW SK_FILES_DONE     /files walked to completion
NEW SK_ABF_PTR    /backfill resolve: walk cursor over (dest, entry) pairs
NEW SK_ABF_SLOT   /backfill resolve: dest slot address to fill
NEW SK_ABF_HASH   /backfill resolve: the recorded name's HT-entry word0 addr
/One slot per file, holding that file's own TIDE_RECORDS_HEAD (Wave 0's
/product) -- so lcount/declare/load each read the same already-built
/chain instead of re-opening and re-tokenizing the file three times
/(see LOAD_TOKENIZE_FILE). 256 is headroom, not a measured count -- the
/project has 42 files today; same margin convention as SK_LCOUNT_BUF
/and friends elsewhere in this file.
NEW SK_FILE_RECORDS_000  Count=256
NEW SK_FILE_RECORDS_BASE  /copy of SK_FILE_RECORDS_000's own value,
                            /same pattern as SK_LCOUNT_BASE/SK_LCOUNT_BUF

── LOAD_MAIN: public entry point ────────────────────────────
NOLINK
/Recompute base pointers from their own _000 self-ref headers, instead of
/inheriting whatever Python's bootstrap already wrote into BS_HT_BASE/
/BS_TOKBUF_BASE/BS_READBUF_BASE. This is the actual self-hosting step for
/those three registers -- LOAD_MAIN deriving its own state independently,
/not leaning on Python having done it first. First, because file reading
/needs all three almost immediately. Was one combined BS_INIT call
/(lexer.re); split into three, one per owning file, so each subsystem
/initializes only its own base pointer.
RVOCA LOAD_MAIN_BSI1 Body=BS_HT_INIT
RVOCA LOAD_MAIN_BSI2 Body=BS_TOKBUF_INIT
RVOCA LOAD_MAIN_BSI3 Body=BS_READBUF_INIT
/Init body buffer
NOVA LOAD_MAIN Op=Move El1=SK_BODY_BUF Exit=SK_BODY_BASE
NOVA LOAD_MAIN_BL Op=Add El1=SK_BODY_BASE El2=SK_BODY_BUF_SIZE Exit=SK_BODY_LIMIT
/Init indent-level stack (LOAD_READ_LINE)
NOVA LOAD_MAIN_ISI Op=Move El1=SK_IND_STACK Exit=SK_IND_STACK_BASE
/Init context stack (PUSH-model block handling)
/Derive slot geometry from CTX_HEADER_SIZE + SK_IND_STACK_SIZE
NOVA LOAD_MAIN_CTDP Op=Add El1=CTX_HEADER_SIZE El2=SK_IND_STACK_SIZE Exit=CTX_OFF_DATA_PTR
NOVA LOAD_MAIN_CTSS Op=Add El1=CTX_OFF_DATA_PTR El2=C_1 Exit=CTX_SLOT_SIZE
NOVA LOAD_MAIN_CTSC Op=Mul El1=CTX_SLOT_SIZE El2=CTX_MAX_DEPTH Exit=SK_CTX_SLOTS_COUNT
/Allocate slot array at runtime (size adapts to SK_IND_STACK_SIZE)
ALLOC_TO LOAD_MAIN_CTA Exit=SK_CTX_SLOTS_BASE Count=SK_CTX_SLOTS_COUNT
CLEAR LOAD_MAIN_CTXSP El1=SK_CTX_SP
/Init backfill
NOVA LOAD_MAIN_BF Op=Move El1=SK_BACKFILL_BUF Exit=SK_BACKFILL_BASE
NOVA Op=Move El1=SK_BACKFILL_BASE Exit=SK_BACKFILL_PTR

/Declare-only allowlist base pointer. The table itself is pre-populated
/by prepare_reca_freeze (loader.py): it scans commands.re for NOVAE
/builders with At=RA_MA0 and NOVA builders with El1=RA_MA0 Exit=RA_MC_LUX,
/discovers every command that creates a named NOVA from its first arg,
/and writes their addresses into SK_DECL_TABLE before LOAD_MAIN runs.
/Idem gives SK_DECL_TABLE's own address (not its word, which Python
/overwrote with the first entry) so the check loop walks correctly.
NOVA LOAD_MAIN_DTB Op=Idem El1=SK_DECL_TABLE Exit=SK_DECL_TABLE_BASE
/Block-opener table base, same story: Python pre-populates SK_OPENER_TABLE
/from the corpus's Open= declarations, overwriting its own word with the
/first entry, so Idem (address-of), not the word, gives the walk base.
/TR_IS_OP (TIDE_RESTRUCTURE) reads it during Wave 0, which runs after this.
NOVA LOAD_MAIN_OTB Op=Idem El1=SK_OPENER_TABLE Exit=SK_OPENER_TABLE_BASE
/Region combo table base (B), same pre-baked-table story: Python fills
/SK_REGION_TABLE from _name_regions before LOAD_MAIN runs; Idem gives its
/address for REGION_LOOKUP to walk. RESERVE_COMBOS (below, before Wave 1)
/turns the Python-baked SK_COMBO_SIZE totals into reserved blocks.
NOVA LOAD_MAIN_RTB Op=Idem El1=SK_REGION_TABLE Exit=SK_REGION_TABLE_BASE
/(SK_SEAL_NAMEPOOL init removed: forward refs no longer copy names into a
/pool -- they record their HT entry address instead. See LOAD_INTERN.)
/Init lcount table
NOVA LOAD_MAIN_LC Op=Move El1=SK_LCOUNT_BUF Exit=SK_LCOUNT_BASE
NOVA Op=Move El1=SK_LCOUNT_BASE Exit=SK_LCOUNT_PTR
/Init nova address registry
NOVA LOAD_MAIN_NAB Op=Move El1=SK_NOVA_ADDRS_BUF Exit=SK_NOVA_ADDRS_BASE
NOVA Op=Move El1=SK_NOVA_ADDRS_BASE Exit=SK_NOVA_ADDRS_PTR
/Init per-file record-chain table (Wave 0 fills it in, one slot per file)
NOVA LOAD_MAIN_FRB Op=Move El1=SK_FILE_RECORDS_000 Exit=SK_FILE_RECORDS_BASE
/Init the indent/body restructure context stack base (TIDE_RESTRUCTURE,
/called per file inside LOAD_TOKENIZE_FILE). Same one-time base-pointer
/derivation as SK_IND_STACK_BASE above; TIDE_RST_SP resets per file, in
/TIDE_RESTRUCTURE itself.
NOVA LOAD_MAIN_RSB Op=Move El1=TIDE_RST_STACK Exit=TIDE_RST_SBASE
/Init pending-LINK buffer

NOVA LOAD_MAIN_PLB Op=Move El1=SK_PLINK_BUF Exit=SK_PLINK_BASE
NOVA Op=Move El1=SK_PLINK_BASE Exit=SK_PLINK_PTR
/Init htable's own name pool bump pointer -- was previously an orphaned
/runtime instruction (HT_NPI_SET, htable.re) that nothing anywhere
/ever called or jumped to, leaving it at 0 and eventually colliding
/with RA_FLUX_BOTTOM's own low address once enough names were stored.
/See BUGS.md Pattern Z.

NOVA LOAD_MAIN_HNPI Op=Move El1=HT_NAME_POOL_BASE Exit=HT_NAME_POOL_PTR
/Reset autolink
CLEAR LOAD_MAIN_MC El1=RA_MC_PREV
/Switch the symbol table BS_LOOKUP/BS_INTERN resolve through to LOAD_MAIN's
/own SK_HTAB. Only BS_HT_BASE needs repointing: BS_HT_SIZE/BS_HT_MASK are
/SET constants (word = 65536/65535 literally) and SK_HTAB_000 was sized
/identically for exactly this reason, so it reuses BS_HT_SIZE/BS_HT_MASK's
/values directly -- no separate SK_HT_SIZE/MASK constants, only the one real
/(NEW, mutable) register needs switching. Every BS_LOOKUP/BS_INTERN call from
/here on, including everything LOAD_MAIN itself calls transitively, resolves
/against SK_HTAB only.
/
/NOT cleared: SK_HTAB arrives pre-seeded with the command vocabulary that
/Python bakes into it (keyword -> handler-symbol lux; see _freeze_prepare's
/SK_HTAB command seed), the same hand-over as SK_DECL_TABLE/SK_OPENER_TABLE/
/SK_REGION_TABLE. Command handlers are stable runtime code, so the vocabulary
/is the consistent core to reuse rather than rebuild; LOAD_MAIN's own name
/interning lands on top of it. Wiping it here (the old HT_CLEAR) is exactly
/what left dispatch with no command words to resolve. The region is otherwise
/freshly-allocated zeros, so no stale entries to guard against.
NOVA LOAD_MAIN_HTSW Op=Move El1=SK_HTAB_000 Exit=SK_HT_BASE
NOVA Op=Move El1=SK_HT_BASE Exit=RA_HT_BASE
NOVA Op=Move El1=BS_HT_SIZE Exit=RA_HT_SIZE
NOVA LOAD_MAIN_HTPT Op=Move El1=SK_HT_BASE Exit=BS_HT_BASE
NOVA LOAD_MAIN_HTJMP Op=Jump Exit=LOAD_MAIN_TOK0
── Wave 0: tokenize — read + strip comments + build the token/record
── chain for every file, once, before lcount/declare/load touch any of
── them (matches Python's own _wave0_parse: read each file once, save
── the result, everything after this reads records, never source text
── again). See LOAD_TOKENIZE_FILE for the per-file work. ────────────
NOLINK
CLEAR LOAD_MAIN_TOK0 El1=SK_FIDX
JEQ LOAD_MAIN_TOKLOOP El1=SK_FIDX El2=BS_FILE_COUNT Exit=LOAD_MAIN_TOKDONE
NOVA LOAD_MAIN_TOKADDR Op=Add El1=BS_FILE_LIST El2=SK_FIDX Exit=SK_TMP
NOVA LOAD_MAIN_TOKLOAD Op=Read El1=SK_TMP Exit=SK_FPATH
RVOCA LOAD_MAIN_TOKF Body=LOAD_TOKENIZE_FILE
NOVA LOAD_MAIN_TOKINC Op=Add El1=SK_FIDX El2=C_1 Exit=SK_FIDX
NOVA LOAD_MAIN_TOKJMP Op=Jump Exit=LOAD_MAIN_TOKLOOP
NOLINK
/Every file is tokenized now. Turn replay on once, for good -- lcount,
/declare and load all read exclusively through it from here on, so it
/never needs to toggle off again (unlike LOAD_TOKENIZE_FILE's own
/TIDE_BUILD_RECORDS call above, which needs live scanning and always
/runs first, while this is still 0).
NOVA LOAD_MAIN_TOKDONE Op=Move El1=C_1 Exit=TIDE_REPLAY_ON
/SEAL/OWN/FREE/ALIAS/SPAN, as bare words, are pre-allocated directly
/by loader.py's own freeze() now (see _bootstrap_only_cmds there), not
/via any source-level anchor here -- a genuine Python-loader
/initialization concern, not a Reca-level one.
/
/There is no "the table starts empty so the first word can't be looked
/up" bootstrap problem here: the symbol table is populated by the time
/the Wave 1 prepass runs, and BS_LOOKUP resolves command words (NEW/SET/
/NOVA/LINK) at that point -- verified empirically. So both the prepass
/(LOAD_PP_ROUTE below) and the main dispatch (LOAD_DISPATCH_LINE) use
/ONE recognizer, BS_LOOKUP -- the Reca analog of Python's single
/`cmd in symbols`/`cmd in _COMMANDS`, used the same way in both its
/prepass and its build passes. BS_HTAB is that table.
── Wave 1: lcount — count LINK lumina per src name ──────────
/TODO (Pattern Z, BUGS.md, agreed with author 2026-07-05, partly done):
/LOAD_PREPASS_FILE below now interns the FOUR primitive declarations
/(LINK count; NEW/SET as 1-lux; NOVA as NOVA_SIZE) -- see LOAD_PP_ROUTE.
/What is still missing vs loader.py's _wave1_lcount_line is the FULLER
/naming set it registers here: the discovered naming_commands
/(RVOCA/RREDI/JEQ/JZ/CLEAR/... -- every macro that names via its first
/arg, from _discover_nova_naming_commands), plus ALIAS targets and
/region/span sizing. Reca reaches those correctly today via Wave 2's
/declare pass + LOAD_APPLY_BACKFILL (forward refs resolved by backfill,
/not by pre-scan) rather than by pre-registering them all here as a
/single declare-before-use barrier. Without a true barrier, Wave 1's
/own interleaved per-file processing has no guarantee that a name
/declared in one file is visible before another file's *use* of it is
/reached (confirmed real, not hypothetical -- see Pattern Z for the full
/trace). This barrier point is deliberately where a real future
/scheduler should fork one thread per file for the declare pass and
/join here -- Wave 1 itself can then also become genuinely parallel,
/safely, since every name it could ever look up already exists by the
/time it starts. Don't skip straight to a fully sequential four-wave
/copy of the Python side -- that throws away the intended future
/overlap between declare and use once real synchronization exists;
/this barrier is the seam where that gets plugged in later.
/Reserve one contiguous block per region combo (B) from the Python-baked
/SK_COMBO_SIZE totals, BEFORE the prepass allocates any name -- so a region
/name's very first allocation already lands in its combo block (ALLOC_COMBO
/via REGION_LOOKUP, in the prepass below), no relocation. Mirrors
/loader.py's _reserve_combo_blocks running before Wave 1 allocates.
RVOCA LOAD_MAIN_RESCB Body=RESERVE_COMBOS
CLEAR LOAD_MAIN_P0 El1=SK_FIDX
JEQ LOAD_MAIN_PLOOP El1=SK_FIDX El2=BS_FILE_COUNT Exit=LOAD_MAIN_PBF
NOVA LOAD_MAIN_PFADDR Op=Add El1=BS_FILE_LIST El2=SK_FIDX Exit=SK_TMP
NOVA LOAD_MAIN_PFLOAD Op=Read El1=SK_TMP Exit=SK_FPATH
RVOCA LOAD_MAIN_PFF Body=LOAD_PREPASS_FILE
NOVA LOAD_MAIN_PINC Op=Add El1=SK_FIDX El2=C_1 Exit=SK_FIDX
NOVA LOAD_MAIN_PJMP Op=Jump Exit=LOAD_MAIN_PLOOP
/Apply backfill *here* too, right after Wave 1 has scanned every
/file once -- the exact same two-phase pattern already used at the
/Wave 2 -> Wave 3 boundary (LOAD_MAIN_DBF below), just one level
/earlier. Wave 1's own SEAL handling (calling LOAD_CMD_SEAL
/during its single scan) defers via the existing kind=2 backfill
/whenever a target isn't registered yet *within this same scan* (e.g.
/SWITCH's own "SEAL SWITCH EXP_SWITCH_START" in expand.re, used by some
/earlier file before this same name's own declaration is reached). Without
/this call, such a deferred alias would stay an unresolved placeholder
/all the way until Wave 2's own backfill call -- too late for
/anything Wave 2 itself needs to Voca/dispatch to. See BUGS.md
/Pattern Z and the SEAL-alias-unification session log.
NOLINK
RVOCA LOAD_MAIN_PBF Body=LOAD_APPLY_BACKFILL
NOVA LOAD_MAIN_PTOW05 Op=Jump Exit=LOAD_MAIN_DSTART
── Wave 2: declare — register every declarable name, every file,
── before Wave 3 (below) touches a single use (Pattern Z, BUGS.md) ────
NOLINK
NOVA LOAD_MAIN_DSTART Op=Move El1=C_1 Exit=SK_DECLARE_ONLY
CLEAR LOAD_MAIN_D0 El1=SK_FIDX
JEQ LOAD_MAIN_DLOOP El1=SK_FIDX El2=BS_FILE_COUNT Exit=LOAD_MAIN_DEND
NOVA LOAD_MAIN_DFADDR Op=Add El1=BS_FILE_LIST El2=SK_FIDX Exit=SK_TMP
NOVA LOAD_MAIN_DFLOAD Op=Read El1=SK_TMP Exit=SK_FPATH
RVOCA LOAD_MAIN_DFILE Body=LOAD_FILE
NOVA LOAD_MAIN_DINC Op=Add El1=SK_FIDX El2=C_1 Exit=SK_FIDX
NOVA LOAD_MAIN_DJMP Op=Jump Exit=LOAD_MAIN_DLOOP
NOLINK
CLEAR LOAD_MAIN_DEND El1=SK_DECLARE_ONLY
/Apply backfill *here*, before Wave 1 starts using anything -- not just
/once at the very end (that spot -- LOAD_MAIN_BF2, below -- still runs
/too, harmless/idempotent, in case Wave 1's own real-use processing adds
/further forward references of its own). Without this, a same-file
/forward reference like "SEAL CHAIN CHAIN_START" (CHAIN_START declared
/later in the same macro body -- the common shape for most of
/expand.re's/synth.re's SEAL registrations) gets correctly *deferred* by LOAD_CMD_SEAL's own
/LOAD_INTERN-based fix, but Wave 1 needs CHAIN callable throughout its
/own single pass across all 42 files -- backfilling only at the very end
/is too late for anything Wave 1 itself needs to call. See BUGS.md
/Pattern Z.
RVOCA LOAD_MAIN_DBF Body=LOAD_APPLY_BACKFILL
NOVA LOAD_MAIN_DTOW1 Op=Jump Exit=LOAD_MAIN_W1
── Wave 1: load — build full graph ──────────────────────────
NOLINK
CLEAR LOAD_MAIN_W1 El1=SK_FIDX
JEQ LOAD_MAIN_LOOP El1=SK_FIDX El2=BS_FILE_COUNT Exit=LOAD_MAIN_BF2
NOVA LOAD_MAIN_FADDR Op=Add El1=BS_FILE_LIST El2=SK_FIDX Exit=SK_TMP
NOVA LOAD_MAIN_FLOAD Op=Read El1=SK_TMP Exit=SK_FPATH
RVOCA LOAD_MAIN_FILE Body=LOAD_FILE
NOVA LOAD_MAIN_INC Op=Add El1=SK_FIDX El2=C_1 Exit=SK_FIDX
NOVA LOAD_MAIN_JMP Op=Jump Exit=LOAD_MAIN_LOOP
/Apply backfill: resolve all forward references
NOLINK
RVOCA LOAD_MAIN_BF2 Body=LOAD_APPLY_BACKFILL
/Apply deferred LINK lumina: now every name's address and prefix are known
RVOCA LOAD_MAIN_PL2 Body=LOAD_APPLY_LINKS
RREDI LOAD_MAIN_RRET

── LOAD_OPEN_FILE: open SK_FPATH O_RDONLY, init read buffer ─
/OUT: RA_LOAD_FD = fd. Leaf.
NOVA LOAD_OPEN_FILE Op=Move El1=SYS_OPENAT Exit=SC_NR
NOVA Op=Move El1=AT_FDCWD Exit=SC_A0
NOVA Op=Move El1=SK_FPATH Exit=SC_A1
NOVA Op=Move El1=O_RDONLY Exit=SC_A2

NOVA LOF_XR Op=Exire El1=C_0 El2=C_0 Exit=C_0
NOVA LOF_FD Op=Move El1=SC_A0 Exit=RA_LOAD_FD
CLEAR LOF_RPOS El1=RA_LOAD_RPOS
CLEAR LOF_RLEN El1=RA_LOAD_RLEN
RREDI LOF_RET

── LOAD_TOKENIZE_FILE: Wave 0's per-file work -- open, strip comments,
── tokenize into a chain, store it, close. Runs once per file, before
── lcount/declare/load ever look at this file (see the Wave 0 loop
── above, LOAD_MAIN). Nothing later re-opens or re-tokenizes it.
NOLINK
RVOCA LOAD_TOKENIZE_FILE Body=LOAD_OPEN_FILE
RVOCA LOAD_TF_TIDE Body=TIDE_FILE
RVOCA LOAD_TF_BREC Body=TIDE_BUILD_RECORDS
/Restructure the flat record chain into indent-nested blocks: openers
/(FOR/SOMA/SWITCH/MATCH/SAVE) keep their bodies on a private chain hung
/off field 2, body records leave the main chain. Same place, same role
/as Python's _parse_indent_blocks call inside _read_re_file -- a step of
/the file-read pipeline, not a separate wave. Updates TIDE_RECORDS_HEAD
/in place, which the store just below then persists for this file.
RVOCA LOAD_TF_RSTC Body=TIDE_RESTRUCTURE
NOVA LOAD_TF_ADDR Op=Add El1=SK_FILE_RECORDS_BASE El2=SK_FIDX Exit=SK_TMP2
NOVA LOAD_TF_STORE Op=Write El1=SK_TMP2 El2=TIDE_RECORDS_HEAD
NOVA LOAD_TF_CLOSE Op=Move El1=SYS_CLOSE Exit=SC_NR
NOVA LOAD_TF_CLFD Op=Move El1=RA_LOAD_FD Exit=SC_A0
NOVA LOAD_TF_CLXR Op=Exire El1=C_0 El2=C_0 Exit=C_0
CLEAR LOAD_TF_RBOFF El1=RA_REDIRECT_BASE
RREDI LOAD_TF_RET

── LOAD_FILE: process one .re file from its own, already-built Wave 0
── record chain (SK_FILE_RECORDS[SK_FIDX]) -- called twice per file
── (declare pass, then load pass), so it resets the replay cursor to
── this chain's head each time rather than rebuilding anything. The
── file itself was already closed by LOAD_TOKENIZE_FILE; this never
── touches disk. LOAD_DISPATCH_LINE's own BS_READ_TOKEN call, and every
── command handler's own internal BS_READ_TOKEN calls further down (the
── name, the op, El1=/El2=/Exit=, ...) all transparently fetch
── pre-tokenized entries instead of re-scanning characters -- see
── BS_READ_TOKEN_REPLAY in lexer.re.
NOLINK
NOVA LOAD_FILE Op=Add El1=SK_FILE_RECORDS_BASE El2=SK_FIDX Exit=SK_TMP2
NOVA LOAD_FO_READ Op=Read El1=SK_TMP2 Exit=TIDE_RECORDS_HEAD
/JZ jumps when the value IS zero, so the empty case is the one that
/skips. This was the other way round -- an empty record list jumped to
/the has-records branch while a real one fell through to the skip -- so
/replay never switched on: TIDE_REPLAY_TOK/REC stayed zero, every
/BS_READ_TOKEN returned an empty token, and LOAD_DISPATCH_CORE's own
/zero-length check bailed out on every line. The run swept all 42 files
/and executed no commands at all.
JZ LOAD_FO_RECCK El1=TIDE_RECORDS_HEAD Exit=LOAD_FO_IND
NOVA LOAD_FO_HASREC Op=Read El1=TIDE_RECORDS_HEAD Exit=TIDE_REPLAY_TOK
NOVA LOAD_FO_SETREC Op=Move El1=TIDE_RECORDS_HEAD Exit=TIDE_REPLAY_REC
CLEAR LOAD_FO_IND El1=SK_IND_DEPTH
/Break the auto-link chain at the file boundary -- mirrors loader.py's
/own _run_pass_over, which calls self._end_chain() (resets _last_nova)
/before every file for the same reason: without this, the last NOVA of
/one file falls through and auto-links to the first NOVA of the next
/one, wiring two unrelated files together. This was never done here;
/only LOAD_MAIN's own one-time reset at bootstrap covered the first file.
CLEAR LOAD_FO_MC El1=RA_MC_PREV
RVOCA LOAD_FL_LOOP Body=LOAD_DISPATCH_LINE
JZ LOAD_FL_EOFCK El1=RA_LOAD_BYTE Exit=LOAD_FL_DONE
NOVA LOAD_FL_JMP Op=Jump Exit=LOAD_FL_LOOP
NOLINK
/One file finished. Counted separately from lines on purpose: "42 files"
/and "27980 lines" answer different questions, and the old meter reported
/only the first while the second was 194.
NOVA LOAD_FL_DONE Op=Add El1=SK_FILES_DONE El2=C_1 Exit=SK_FILES_DONE
RREDI LOAD_FL_RRET

── LOAD_PREPASS_FILE: scan file, count LINK lumina per src ───
/Reads from this file's own Wave 0 record chain (SK_FILE_RECORDS
/[SK_FIDX]), the same one declare/load read -- see LOAD_TOKENIZE_FILE.
/Never opens the file itself: tide already read, stripped and
/tokenized it once, before this ever runs.
/
/Used to also carry its own hand-rolled "peek first byte, seed hash,
/call LOAD_DL_COLLECT" dance -- a second, independent reimplementation
/of exactly what BS_READ_TOKEN already does (whitespace-skip including
/SP/TAB, not just LF; token collection with the same hash formula).
/Simplified to call BS_READ_TOKEN directly: it already skips leading
/indentation as part of its normal whitespace-skip (no separate
/SP/TAB-means-skip-the-whole-line special case needed -- an indented
/continuation line's own first real word, e.g. "Move", still correctly
/fails to match any registered name in the MATCH routing below and
/falls through Except to the same "skip rest of line" path, just
/reached uniformly instead of via a second, hand-duplicated route).
/BS_READ_TOKEN itself doesn't care whether it's replaying or scanning
/live -- same call, same contract either way (BS_READ_TOKEN_REPLAY,
/lexer.re) -- so nothing below this setup needed to change for it.
NOLINK
NOVA LOAD_PREPASS_FILE Op=Add El1=SK_FILE_RECORDS_BASE El2=SK_FIDX Exit=SK_TMP2
NOVA LOAD_PP_READ Op=Read El1=SK_TMP2 Exit=TIDE_RECORDS_HEAD
JZ LOAD_PP_RECCK El1=TIDE_RECORDS_HEAD Exit=LOAD_PP_JMP2LP
NOVA LOAD_PP_HASREC Op=Read El1=TIDE_RECORDS_HEAD Exit=TIDE_REPLAY_TOK
NOVA LOAD_PP_SETREC Op=Move El1=TIDE_RECORDS_HEAD Exit=TIDE_REPLAY_REC
NOVA LOAD_PP_JMP2LP Op=Jump Exit=LOAD_PP_LOOP
NOLINK
RVOCA LOAD_PP_LOOP Body=BS_READ_TOKEN
JZ LOAD_PP_TLENCK El1=RA_LOAD_TLEN Exit=LOAD_PP_MAYBE_EOF
/Route the token by name via MATCH -- a byte-comparison tree, NOT a symbol-
/table lookup. This is deliberate and load-bearing: the prepass runs at
/Wave 1, when LOAD_MAIN's own symbol table (SK_HTAB) is still EMPTY -- it is
/the pass that FIRST interns names. A BS_LOOKUP here would miss every command
/word (verified at runtime: with a BS_LOOKUP version LOAD_MAIN swept all 41
/files but interned nothing, HT 0 entries, every line skipped), because the
/word it needs is not registered yet. MATCH compares the raw token bytes and
/needs no table, so it bootstraps recognition from nothing -- the four
/PRIMITIVE declaration commands (LINK counts a lumen; NEW/SET size a 1-lux
/name; NOVA sizes a NOVA_SIZE stub). Anything else falls through Except to
/LOAD_PP_SKIP. (An earlier attempt replaced this with BS_LOOKUP + Idem on the
/theory the table was already populated -- true at Python COMPILE time, false
/at Reca RUN time. The distinction is the whole point of this comment.)
NOVA LOAD_PP_B0 Op=Move El1=BS_TOKBUF_BASE Exit=RA_MATCH_BASE

MATCH LOAD_PP_ROUTE Except=LOAD_PP_SKIP
    LINK  LOAD_PP_LINK
    NEW   LOAD_PP_DECL1
    NOVA  LOAD_PP_NOVADCL
    SET   LOAD_PP_DECL1
/LINK found: read src token, skip if templated, increment its lcount
NOLINK
RVOCA LOAD_PP_LINK Body=BS_READ_TOKEN
RVOCA LOAD_PP_LTCK Body=LOAD_PP_TMPL_CHECK
JZ LOAD_PP_LTCJ El1=SK_TMP Exit=LOAD_PP_LINTERN
NOVA LOAD_PP_LTSKJ Op=Jump Exit=LOAD_PP_SKIP
NOLINK
RVOCA LOAD_PP_LINTERN Body=BS_INTERN
NOVA LOAD_PP_LHK Op=Move El1=RA_BS_RESULT Exit=RA_LOAD_HASH
RVOCA LOAD_PP_INC Body=LOAD_LCOUNT_INC
NOVA LOAD_PP_IJMP Op=Jump Exit=LOAD_PP_SKIP
NOLINK
/NEW/SET/NOVA found: read the declared name, skip if templated (FOR body
/lines contain {X}/{N} placeholders — interning them pollutes the symbol
/table with junk entries; Python's _wave1_lcount_line skips the same way),
/then intern it as a placeholder sized for what it'll become — 1 lux for
/a plain name (NEW/SET), NOVA_SIZE for a NOVA stub (forward-reference-safe
/pre-sizing: so if some OTHER line references this name before reaching
/its real declaration, it finds a correctly-sized entry already waiting
/instead of an undersized 1-lux one — see BUGS.md).
RVOCA LOAD_PP_DECL1 Body=LOAD_READ_NAME
NOVA LOAD_PP_D1CNT Op=Move El1=C_1 Exit=RA_BS_INT_COUNT
NOVA LOAD_PP_D1J Op=Jump Exit=LOAD_PP_DCORE
NOLINK
RVOCA LOAD_PP_NOVADCL Body=LOAD_READ_NAME
NOVA LOAD_PP_DNCNT Op=Move El1=NOVA_SIZE Exit=RA_BS_INT_COUNT
NOVA LOAD_PP_DNJ Op=Jump Exit=LOAD_PP_DCORE
NOLINK
RVOCA LOAD_PP_DCORE Body=LOAD_PP_TMPL_CHECK
JZ LOAD_PP_DTCJ El1=SK_TMP Exit=LOAD_PP_DINTERN
NOVA LOAD_PP_DTSKJ Op=Jump Exit=LOAD_PP_SKIP
NOLINK
NOVA LOAD_PP_DINTERN Op=Move El1=C_1 Exit=RA_BS_INT_SELFREF
/Region (B): if this name carries a combo (REGION_LOOKUP by its hash finds
/it in the Python-baked SK_REGION_TABLE), allocate it from that combo's
/reserved block -- threaded to BS_INTERN_AT via the one-shot RA_BS_INT_COMBO.
/0 (the common case) means the ordinary flat bump. RA_LOAD_HASH still holds
/this name's hash (LOAD_READ_NAME set it; TMPL_CHECK left it untouched), and
/BS_INTERN_AT reads it again for its own lookup.
RVOCA LOAD_PP_RGN Body=REGION_LOOKUP
NOVA LOAD_PP_RGNSET Op=Move El1=RA_CB_MASK Exit=RA_BS_INT_COMBO
RVOCA LOAD_PP_DI Body=BS_INTERN_AT
NOVA LOAD_PP_DIJ Op=Jump Exit=LOAD_PP_SKIP
NOLINK
//
Calls BS_SKIP_TO_EOL directly — that routine is itself now a safe no-op
when already at LF/EOF (see lexer.re), so no separate guard is needed here.
//
RVOCA LOAD_PP_SKIP Body=BS_SKIP_TO_EOL
NOVA LOAD_PP_SJMP Op=Jump Exit=LOAD_PP_LOOP

NOLINK
── LOAD_PP_MAYBE_EOF: no token found -- true EOF, or just an empty/
── whitespace-only line (RA_LOAD_BYTE tells which, same distinction
── BS_READ_TOKEN's own callers always make: 0 vs LF).
NOVA LOAD_PP_MAYBE_EOF Op=Move El1=RA_LOAD_BYTE Exit=SK_TMP

JZ LOAD_PP_EOFCK El1=SK_TMP Exit=LOAD_PP_CLOSE
NOVA LOAD_PP_ELJMP Op=Jump Exit=LOAD_PP_LOOP

NOLINK
RREDI LOAD_PP_CLOSE

── LOAD_LCOUNT_INC: increment LINK count for src name ────────
//
IN: RA_LOAD_HASH = intern addr of src name.
Entry format: [hash, count, hash, count, ..., 0]. Stride 2.
//
NOLINK
NOVA LOAD_LCOUNT_INC Op=Move El1=SK_LCOUNT_BASE Exit=SK_TMP
NOVA LOAD_LCI_LOOP Op=Read El1=SK_TMP Exit=SK_TMP2
JZ LOAD_LCI_ZCKL El1=SK_TMP2 Exit=LOAD_LCI_NEW
JEQ LOAD_LCI_MCK El1=SK_TMP2 El2=RA_LOAD_HASH Exit=LOAD_LCI_HIT
NOVA LOAD_LCI_ADV Op=Add El1=SK_TMP El2=C_2 Exit=SK_TMP
NOVA LOAD_LCI_JMP Op=Jump Exit=LOAD_LCI_LOOP
NOLINK
NOVA LOAD_LCI_HIT Op=Add El1=SK_TMP El2=C_1 Exit=SK_TMP2
NOVA LOAD_LCI_RD Op=Read El1=SK_TMP2 Exit=SK_FLAG
NOVA LOAD_LCI_INC Op=Add El1=SK_FLAG El2=C_1 Exit=SK_FLAG
NOVA LOAD_LCI_WR Op=Write El1=SK_TMP2 El2=SK_FLAG
RREDI LOAD_LCI_RRET
NOVA LOAD_LCI_NEW Op=Write El1=SK_LCOUNT_PTR El2=RA_LOAD_HASH
NOVA Op=Add El1=SK_LCOUNT_PTR El2=C_1 Exit=SK_TMP2
NOVA Op=Write El1=SK_TMP2 El2=C_1
NOVA Op=Add El1=SK_LCOUNT_PTR El2=C_2 Exit=SK_LCOUNT_PTR
NOVA Op=Write El1=SK_LCOUNT_PTR El2=C_0
RREDI LOAD_LCI_NRRET

── LOAD_LCOUNT_GET: get LINK count for name → SK_TMP ─────────
//
OUT: SK_TMP = count (0 if miss).
//
NOLINK
NOVA LOAD_LCOUNT_GET Op=Move El1=SK_LCOUNT_BASE Exit=SK_TMP2
NOVA LOAD_LCG_LOOP Op=Read El1=SK_TMP2 Exit=SK_TMP
JZ LOAD_LCG_ZCKL El1=SK_TMP Exit=LOAD_LCG_MISS
JEQ LOAD_LCG_MCK El1=SK_TMP El2=RA_LOAD_HASH Exit=LOAD_LCG_HIT
NOVA LOAD_LCG_ADV Op=Add El1=SK_TMP2 El2=C_2 Exit=SK_TMP2
NOVA LOAD_LCG_JMP Op=Jump Exit=LOAD_LCG_LOOP
NOLINK
NOVA LOAD_LCG_HIT Op=Add El1=SK_TMP2 El2=C_1 Exit=SK_TMP2
NOVA LOAD_LCG_RD Op=Read El1=SK_TMP2 Exit=SK_TMP
RREDI LOAD_LCG_RRET
NOLINK
CLEAR LOAD_LCG_MISS El1=SK_TMP
RREDI LOAD_LCG_MRRET

── NOVA_ADDRS_CHECK: set RA_LM_OFFSET from nova registry ────
//
IN:  RA_LM_SRC = lux address to check.
OUT: RA_LM_OFFSET = NOVA_SIZE if RA_LM_SRC is a NOVA, C_1 if Data.
Python parity: `base = _PAD if src_addr in self._nova_addrs_set else _DATA_OFF`
//
NOLINK
NOVA NOVA_ADDRS_CHECK Op=Move El1=SK_NOVA_ADDRS_BASE Exit=SK_TMP2
NOVA NAC_LOOP Op=Read El1=SK_TMP2 Exit=SK_TMP
JZ NAC_ZCK El1=SK_TMP Exit=NAC_MISS
JEQ NAC_MCK El1=SK_TMP El2=RA_LM_SRC Exit=NAC_HIT
NOVA NAC_ADV Op=Add El1=SK_TMP2 El2=C_1 Exit=SK_TMP2
NOVA NAC_JMP Op=Jump Exit=NAC_LOOP
NOLINK
NOVA NAC_HIT Op=Move El1=NOVA_SIZE Exit=RA_LM_OFFSET
RREDI NAC_HRET
NOLINK
NOVA NAC_MISS Op=Move El1=C_1 Exit=RA_LM_OFFSET
RREDI NAC_MRET

── LOAD_APPLY_BACKFILL: resolve forward references ────────────
//
Walks the deferred buffer of (dest_slot, entry_addr) pairs written by
LOAD_INTERN's miss path (parser.re). Each pair says "operand cell
dest_slot references the name whose HT entry is at entry_addr". By now
the name's declaration (if any) has upserted its located lid into that
same entry, so the current lid is read straight out of it -- no lookup,
no stored name. word0 = (hash<<32)|lid; the low 32 bits are the lid (0
if the name was never declared, same not-resolved outcome as before).

Reset at the end: the pairs are consumed here, and each wave re-dispatches
every file and re-records its own forward refs, so nothing needs to
survive across the three calls (LOAD_MAIN_PBF/DBF/BF2). Resetting bounds
the buffer to one wave's refs instead of letting it grow unbounded.

Explicit jump to LOAD_ABF_LOOP (not fall-through): relied on address
adjacency once and landed on an unrelated symbol when allocation order
shifted -- see BUGS.md Pattern Z.
//
NOLINK
NOVA LOAD_APPLY_BACKFILL Op=Move El1=SK_BACKFILL_BASE Exit=SK_ABF_PTR
NOVA LOAD_ABF_ENTRYJ Op=Jump Exit=LOAD_ABF_LOOP
NOLINK
/dest_slot (record word 0); 0 = terminator (dest is always a real addr)
NOVA LOAD_ABF_LOOP Op=Read El1=SK_ABF_PTR Exit=SK_ABF_SLOT
JZ LOAD_ABF_CK0 El1=SK_ABF_SLOT Exit=LOAD_ABF_DONE
/entry_addr (record word 1) = the name's HT entry word0 address
NOVA LOAD_ABF_EADDR Op=Add El1=SK_ABF_PTR El2=C_1 Exit=SK_TMP
NOVA LOAD_ABF_EREAD Op=Read El1=SK_TMP Exit=SK_ABF_HASH
/read the entry's word0 and extract the located lid (low 32 bits)
NOVA LOAD_ABF_WVAL Op=Read El1=SK_ABF_HASH Exit=SK_ABF_WVAL
NOVA LOAD_ABF_LID Op=And El1=SK_ABF_WVAL El2=MASK_LOW32 Exit=SK_ABF_LID
/write the resolved lid into the operand cell
NOVA LOAD_ABF_WRITE Op=Write El1=SK_ABF_SLOT El2=SK_ABF_LID
NOVA LOAD_ABF_NEXT Op=Add El1=SK_ABF_PTR El2=C_2 Exit=SK_ABF_PTR
NOVA LOAD_ABF_JMP Op=Jump Exit=LOAD_ABF_LOOP
NOLINK
/consumed -- reset the write cursor and sentinel so refs don't accumulate
NOVA LOAD_ABF_RST Op=Move El1=SK_BACKFILL_BASE Exit=SK_BACKFILL_PTR
NOVA LOAD_ABF_RSTSEN Op=Write El1=SK_BACKFILL_PTR El2=C_0
RREDI LOAD_ABF_DONE

── LOAD_APPLY_LINKS: apply deferred LINK lumina after Wave 1 ──
//
By now every name's address is resolved (via symtab indirection). Walks
SK_PLINK_BUF, resolves each (src,rel,tgt) triple, determines lux kind via
NOVA_ADDRS_CHECK (Python parity: _nova_addrs_set), and calls ADD_LUMEN.
//
NOLINK
NOVA LOAD_APPLY_LINKS Op=Move El1=SK_PLINK_BASE Exit=SK_PLINK_ITER
NOVA LOAD_APL_LOOP Op=Read El1=SK_PLINK_ITER Exit=SK_TMP /src symtab addr (0=terminator)
JZ LOAD_APL_CK0 El1=SK_TMP Exit=LOAD_APL_DONE
NOVA LOAD_APL_RSRC Op=Read El1=SK_TMP Exit=RA_LM_SRC /resolved src addr
NOVA LOAD_APL_A1 Op=Add El1=SK_PLINK_ITER El2=C_1 Exit=SK_TMP2
NOVA LOAD_APL_RRELA Op=Read El1=SK_TMP2 Exit=SK_TMP2 /rel symtab addr
NOVA LOAD_APL_RREL Op=Read El1=SK_TMP2 Exit=RA_LM_REL /resolved rel addr
NOVA LOAD_APL_A2 Op=Add El1=SK_PLINK_ITER El2=C_2 Exit=SK_TMP3
NOVA LOAD_APL_RTGTA Op=Read El1=SK_TMP3 Exit=SK_TMP3 /tgt symtab addr
NOVA LOAD_APL_RTGT Op=Read El1=SK_TMP3 Exit=RA_LM_EXIT /resolved tgt addr
RVOCA LOAD_APL_NAC Body=NOVA_ADDRS_CHECK
RVOCA LOAD_APL_ADD Body=ADD_LUMEN
NOVA LOAD_APL_NEXT Op=Add El1=SK_PLINK_ITER El2=C_3 Exit=SK_PLINK_ITER
NOVA LOAD_APL_JMP Op=Jump Exit=LOAD_APL_LOOP
NOLINK
RREDI LOAD_APL_DONE

