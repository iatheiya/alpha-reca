/lexer.re — Tokenizer for the Reca loader
/
/Turns a byte stream into tokens over .re source files. Does not read
/the bytes itself: BS_READ_BYTE and the read buffer live in io.re, and
/comment handling lives in comments.re. This file owns exactly one job
/-- turning bytes into tokens.
/
/Functions:
/  BS_TOKBUF_INIT  — initialize this file's own base pointer (BS_TOKBUF_000)
/  BS_SKIP_TO_EOL  — consume bytes until LF or EOF
/  BS_READ_TOKEN   — read next whitespace-delimited token into BS_TOKBUF
/  BS_TOKEN_VALUE  — strip "key=" prefix from current tokbuf, recompute hash
/  BS_SCAN_EQ      — scan tokbuf for '=', set RA_BS_FLAG=1 if found
/  BS_PACK_TOKBUF  — pack current token bytes into dense packed string
/  BS_UNPACK_TOKBUF — inverse: dense packed string back into BS_TOKBUF_BASE
/  BS_PARSE_INT    — parse integer from tokbuf → RA_BS_TMP2
/
/DEPENDS ON: aspects.re constants.re ascii.re registers.re linux_generic.re
/            io.re (BS_READ_BYTE — the byte source)
/            alloc.re (BS_PACK_TOKBUF uses ALLOC)
/            intern.re (BS_HT_MASK for hash masking, RA_BS_TMP/FLAG/RESULT)

/── Token buffer ──────────────────────────────────────────────
/Holds the current token bytes, 1 byte per lux, 0-terminated.
/RA_LOAD_TLEN = number of valid bytes. RA_LOAD_HASH = djb2 hash.
SET BS_TOKBUF_SIZE Value=256
NEW BS_TOKBUF_000  Count=256
NEW    BS_TOKBUF_BASE  /base lux addr (written by BS_TOKBUF_INIT)
/RA_LOAD_TLEN, RA_LOAD_BYTE, RA_LOAD_HASH — declared in registers.re

/── Pack-tokbuf scratch ───────────────────────────────────────
NEW RA_BS_PACK_WORD  /accumulator: current u64 word being built
NEW RA_BS_PACK_SHIFT  /bit shift for next byte (0, 8, 16 ... 56)
NEW RA_BS_PACK_SIDX  /source index into tokbuf (0..tlen-1)
NEW RA_BS_PACK_DST  /destination lux pointer in aether

/── Shared scratch (also used by intern.re and wax.re) ───────
NEW RA_BS_TMP
NEW RA_BS_TMP2
NEW RA_BS_TMP3
NEW RA_BS_FLAG
/Dedicated result register for BS_PARSE_INT — isolated from RA_BS_TMP2 which
/is reused as scratch by BS_READ_TOKEN / BS_TOKEN_VALUE hash loops.
NEW RA_BS_PIVAL
SET RA_BS_FLAG /self-ref: Equal writes here, JumpIf reads here

/── djb2 initial value ────────────────────────────────────────
SET BS_HASH0 Value=5381

/── BS_TOKBUF_INIT: initialize this file's own base pointer ────
/Must be called once before BS_READ_TOKEN. Reads BS_TOKBUF_000's own
/self-ref word to get its lux address. Was part of a single, three-
/subsystem BS_INIT (htable/intern.re, tokbuf/here, readbuf/io.re) --
/split so each file initializes only its own base pointer, matching
/this file's own stated scope ("owns exactly one job"). Called by
/LOAD_MAIN (wax.re) alongside BS_HT_INIT (intern.re) and
/BS_READBUF_INIT (io.re).
NOVA BS_TOKBUF_INIT Op=Move El1=BS_TOKBUF_000 Exit=BS_TOKBUF_BASE

RREDI BS_TOKBUF_INIT_RET

/── BS_SKIP_TO_EOL: consume bytes until LF or EOF ─────────────
NOLINK
SET BS_SKIP_TO_EOL Value=BS_SKIP_EOL /alias: both names point here
//
In replay a line IS a record, and its end is known without reading
anything: drop whatever tokens this record still has and report that the
line ended.

Scanning for a byte here is what stalled the self-hosted run. tide
swallows the whole file up front, so by the time a command handler had
read its arguments and asked to skip to end of line, the byte stream was
long drained. It answered 0 -- end of FILE -- and the file loop closed
the file after one or two lines. Token reading had been moved onto
records; ending a line had not, so the two halves of the same read were
looking at different places. Traced end to end: handler -> skip to end
of line -> read byte -> EOF -> file closed.
//
JZ BS_SKIP_EOL El1=TIDE_REPLAY_ON Exit=BS_SE_GUARD
NOVA BS_SE_RDROP Op=Move El1=C_0 Exit=TIDE_REPLAY_TOK
NOVA BS_SE_RLF Op=Move El1=LF Exit=RA_LOAD_BYTE
NOVA BS_SE_RJ Op=Jump Exit=BS_SE_DONE
NOLINK
/Guard: if the caller's last token-read already stopped exactly at LF/EOF
/(e.g. a token immediately followed by newline, no trailing inline
/comment), scanning forward here would search for the *next* LF — i.e.
/it would silently consume the ENTIRE FOLLOWING LINE, since this routine
/has no notion of "already at a line boundary", only "find the next LF".
JZ BS_SE_GUARD El1=RA_LOAD_BYTE Exit=BS_SE_DONE
JEQ BS_SE_GLFCK El1=RA_LOAD_BYTE El2=LF Exit=BS_SE_DONE
NOVA BS_SE_GJMP Op=Jump Exit=BS_SE_RD
NOLINK
RVOCA BS_SE_RD Body=BS_READ_BYTE
JZ BS_SE_EOF El1=RA_LOAD_BYTE Exit=BS_SE_DONE
JEQ BS_SE_LF El1=RA_LOAD_BYTE El2=LF Exit=BS_SE_DONE
NOVA BS_SE_LOOP Op=Jump Exit=BS_SE_RD
NOLINK
RREDI BS_SE_DONE

/── BS_READ_TOKEN: read next whitespace-delimited token ────────
/Skips leading whitespace (SP, TAB, CR). Stops at LF or EOF.
/
/Knows nothing about comments, by design. tide (wax.re) strips them
/before any byte reaches here, so a slash is just a character. This
/function used to carry its own comment detection — three separate
/copies of a rule that comments.re already owned, each of which got it
/wrong at some point. They are gone; do not add them back. If a comment
/ever reaches this function, the bug is upstream in tide, not here.
/OUT: BS_TOKBUF_BASE[0..RA_LOAD_TLEN-1] = token bytes
/     RA_LOAD_HASH = djb2 hash masked to BS_HT_MASK
/     RA_LOAD_BYTE = 0 (EOF), LF (end of line), or terminator byte
NEW TIDE_REPLAY_ON In=OWN  /0 = normal byte scanning (unchanged below);
                            /else = fetch pre-tokenized entries instead
                            /(see BS_READ_TOKEN_REPLAY, wax.re's own
                            /record chain). Off for every file today --
                            /this is one cheap comparison ahead of the
                            /existing scan, not a behavior change.
NEW BS_RT_INQUOTE In=OWN   /1 while collecting inside a "..." span, so inner
                            /whitespace is kept and only the closing quote
                            /ends the token -- the tokenizer's own copy of
                            /_tokenize_quoted's quote rule (loader.py).
NOLINK
JZ BS_READ_TOKEN El1=TIDE_REPLAY_ON Exit=BS_RT_NORMAL
RVOCA BS_RT_REPLAY_CALL Body=BS_READ_TOKEN_REPLAY
RREDI BS_RT_REPLAY_RET
NOLINK
NOVA BS_RT_NORMAL Op=Move El1=C_0 Exit=RA_LOAD_TLEN

NOVA BS_RT_HASH0 Op=Move El1=BS_HASH0 Exit=RA_LOAD_HASH
NOVA BS_RT_JMP2SK Op=Jump Exit=BS_RT_SKP
NOLINK
NOVA BS_RT_SKP Op=Jump Exit=BS_RT_SKP_RD
NOLINK
NOVA BS_RT_SKP_RD Op=Jump Exit=BS_RT_SKP_RD2
NOLINK
RVOCA BS_RT_SKP_RD2 Body=BS_READ_BYTE
JZ BS_RT_EOF El1=RA_LOAD_BYTE Exit=BS_RT_DONE
JEQ BS_RT_LF El1=RA_LOAD_BYTE El2=LF Exit=BS_RT_DONE
SWITCH RA_LOAD_BYTE
    SP    BS_RT_SKP_RD2
    TAB   BS_RT_SKP_RD2
    CR    BS_RT_SKP_RD2

NOVA BS_RT_COL Op=Jump Exit=BS_RT_COLLECT
NOLINK
/BS_RT_INQUOTE tracks whether we are inside a "..." span. It starts 0 and
/toggles on EVERY double-quote byte as that byte is collected (BS_RT_QTOG
/below) -- so a quote anywhere keeps its whole span together, whether it
/opens the token ("a b c") or sits mid-token (Value="a b"). Mirrors
/_tokenize_quoted, which was fixed the same way: a quote need not be the
/token's first byte to start a quoted span, else every keyed string value
/with a space (SF_LIMITER_BARE = "Bare Lux: " and ~15 more) got truncated
/at its first inner space.
NOVA BS_RT_COLLECT Op=Move El1=C_0 Exit=BS_RT_INQUOTE
/Enter the collection loop. Without this jump BS_RT_COLLECT (a Move that only
/inits BS_RT_INQUOTE) fell straight into the NOLINK below -- next=LUX_END --
/so the loop was never started and every self-hosted tokenize ended on the
/first non-whitespace byte (SK_FIDX stuck at 0). Same Jump-then-NOLINK loop-
/entry idiom every other loop head in this file uses (BS_TV_LOOP, BS_SE2_LOOP,
/...): the explicit jump enters, the NOLINK keeps BS_RT_COL2 a clean head the
/loop-back edges (BS_RT_INQCK / BS_RT_MORE) also target.
NOVA BS_RT_COLLECT_J Op=Jump Exit=BS_RT_COL2
NOLINK
/Capacity guard, at the loop head: never collect past BS_TOKBUF_SIZE.
/BS_RT_STORE writes into a fixed 256-cell buffer (NEW BS_TOKBUF_000
/Count=256) with nothing structurally stopping the write from running off
/the end into whatever is allocated after it -- and something IS: the very
/next luces in this file (BS_READ_TOKEN's own body). An over-long read
/therefore used to overwrite this function's own code mid-build, one of
/those clobbered luces being BS_RT_NORMAL (the RA_LOAD_TLEN reset), which
/turned a single overrun into a cascade. This bound is the stop -- the same
/universal safety invariant ALLOC's own count guard is (see _alloc,
/loader.py): a buffer write that cannot exceed the buffer, whatever the
/input. A token at the cap is truncated, not allowed to corrupt memory;
/no real token in the corpus approaches 256 bytes (the longest are divider
/runs and symbol names, all far shorter).
NOVA BS_RT_COL2 Op=ULess El1=RA_LOAD_TLEN El2=BS_TOKBUF_SIZE Exit=RA_JEQ_FLAG
NOVA BS_RT_CAPJ Op=JumpIf El1=RA_JEQ_FLAG Exit=BS_RT_STADDR
NOVA BS_RT_CAPDONE Op=Jump Exit=BS_RT_DONE
NOLINK
NOVA BS_RT_STADDR Op=Add El1=BS_TOKBUF_BASE El2=RA_LOAD_TLEN Exit=RA_BS_TMP
NOVA BS_RT_STORE Op=Write El1=RA_BS_TMP El2=RA_LOAD_BYTE
HASH_STEP BS_RT_H33 RA_LOAD_HASH RA_LOAD_BYTE RA_BS_TMP2 RA_LOAD_HASH
NOVA BS_RT_INC Op=Add El1=RA_LOAD_TLEN El2=C_1 Exit=RA_LOAD_TLEN
/The byte just stored was a quote -> flip in/out of quoted mode. The quote
/itself stays in the token (it was already stored above); flipping here,
/while RA_LOAD_BYTE still holds it, means the NEXT byte read below is judged
/with the correct in/out state. Equal-against-0 is the flip: 0->1, 1->0.
JEQ BS_RT_QTOG El1=RA_LOAD_BYTE El2=DQUOTE Exit=BS_RT_QFLIP
NOVA BS_RT_QTOGJ Op=Jump Exit=BS_RT_NB
NOLINK
NOVA BS_RT_QFLIP Op=Equal El1=BS_RT_INQUOTE El2=C_0 Exit=BS_RT_INQUOTE
/After flipping quote mode, converge with the non-quote path at BS_RT_NB --
/symmetric to BS_RT_QTOGJ's own `Jump Exit=BS_RT_NB` above. Without it the
/flip (an Equal that only updates BS_RT_INQUOTE) fell into the NOLINK and
/terminated (next=LUX_END): every token containing a '"' died mid-collect.
NOVA BS_RT_QFLIP_J Op=Jump Exit=BS_RT_NB
NOLINK
RVOCA BS_RT_NB Body=BS_READ_BYTE
/EOF and LF end the token in every mode.
JZ BS_RT_EOFCK El1=RA_LOAD_BYTE Exit=BS_RT_DONE
JEQ BS_RT_LFCK El1=RA_LOAD_BYTE El2=LF Exit=BS_RT_DONE
/Inside a quoted span, whitespace is content -- keep collecting.
NOVA BS_RT_INQCK Op=JumpIf El1=BS_RT_INQUOTE Exit=BS_RT_COL2
/Outside quotes, any whitespace ends the token.
JEQ BS_RT_SPCK El1=RA_LOAD_BYTE El2=SP Exit=BS_RT_DONE
JEQ BS_RT_TABCK El1=RA_LOAD_BYTE El2=TAB Exit=BS_RT_DONE
JEQ BS_RT_CRCK El1=RA_LOAD_BYTE El2=CR Exit=BS_RT_DONE
NOVA BS_RT_MORE Op=Jump Exit=BS_RT_COL2
NOLINK
RREDI BS_RT_DONE

NEW TIDE_REPLAY_REC In=OWN  /current record being replayed (0 = none set up)
NEW TIDE_REPLAY_TOK In=OWN  /current token within it (0 = record's tokens exhausted)
NEW BS_RTR_TMP In=OWN
NEW BS_RTR_TMP2 In=OWN
NEW BS_RTR_HSAVE In=OWN  /token's hash, stashed across the BS_UNPACK_TOKBUF call
NEW BS_RTR_NSAVE In=OWN  /token's own next-pointer, stashed the same way

/── BS_READ_TOKEN_REPLAY: fetch a pre-tokenized entry instead of scanning ──
/IN:  TIDE_REPLAY_TOK/TIDE_REPLAY_REC (set by whoever starts a replay --
/     not this function's job to initialize them)
/OUT: same contract as BS_READ_TOKEN itself -- BS_TOKBUF_BASE, RA_LOAD_TLEN,
/     RA_LOAD_HASH, RA_LOAD_BYTE (0=EOF, LF=end of this record/line, else
/     a real token was found)
NOLINK
NOVA BS_READ_TOKEN_REPLAY Op=Move El1=TIDE_REPLAY_TOK Exit=BS_RTR_TMP

JZ BS_RTR_TOKCK El1=BS_RTR_TMP Exit=BS_RTR_NOTOK
/A real token is next -- read every field BEFORE calling BS_UNPACK_TOKBUF,
/which uses several of its own RA_BS_TMP*/RA_BS_PACK_* scratch registers
/internally and would otherwise clobber whatever we still needed from
/this token's own address.
NOVA BS_RTR_PACKED Op=Read El1=BS_RTR_TMP Exit=RA_BS_TMP2
NOVA BS_RTR_LADDR Op=Add El1=BS_RTR_TMP El2=C_1 Exit=BS_RTR_TMP2
NOVA BS_RTR_LEN Op=Read El1=BS_RTR_TMP2 Exit=RA_LOAD_TLEN
NOVA BS_RTR_HADDR Op=Add El1=BS_RTR_TMP El2=C_2 Exit=BS_RTR_TMP2
NOVA BS_RTR_HASH Op=Read El1=BS_RTR_TMP2 Exit=BS_RTR_HSAVE
NOVA BS_RTR_NADDR Op=Add El1=BS_RTR_TMP El2=C_3 Exit=BS_RTR_TMP2
NOVA BS_RTR_NEXT Op=Read El1=BS_RTR_TMP2 Exit=BS_RTR_NSAVE
RVOCA BS_RTR_UNPACK Body=BS_UNPACK_TOKBUF
NOVA BS_RTR_HSET Op=Move El1=BS_RTR_HSAVE Exit=RA_LOAD_HASH
NOVA BS_RTR_NSET Op=Move El1=BS_RTR_NSAVE Exit=TIDE_REPLAY_TOK
NOVA BS_RTR_BSET Op=Move El1=SP Exit=RA_LOAD_BYTE
RREDI BS_RTR_DONE1

NOLINK
── BS_RTR_NOTOK: this record's tokens are exhausted -- is there a next record?
NOVA BS_RTR_NOTOK Op=Move El1=TIDE_REPLAY_REC Exit=BS_RTR_TMP

JZ BS_RTR_RECCK El1=BS_RTR_TMP Exit=BS_RTR_TRUEEOF
NOVA BS_RTR_RNADDR Op=Add El1=BS_RTR_TMP El2=C_1 Exit=BS_RTR_TMP2
NOVA BS_RTR_RNEXT Op=Read El1=BS_RTR_TMP2 Exit=BS_RTR_TMP2
JZ BS_RTR_NRCK El1=BS_RTR_TMP2 Exit=BS_RTR_TRUEEOF
/there is a next record -- report "end of this one" now, advance for next call
NOVA BS_RTR_ADVREC Op=Move El1=BS_RTR_TMP2 Exit=TIDE_REPLAY_REC
NOVA BS_RTR_ADVTOK Op=Read El1=BS_RTR_TMP2 Exit=TIDE_REPLAY_TOK
CLEAR BS_RTR_TL0 El1=RA_LOAD_TLEN
NOVA BS_RTR_LFSET Op=Move El1=LF Exit=RA_LOAD_BYTE
RREDI BS_RTR_DONE2

NOLINK
── BS_RTR_TRUEEOF: no more records at all -- genuine end of file ──
NOVA BS_RTR_TRUEEOF Op=Move El1=C_0 Exit=RA_LOAD_TLEN

NOVA BS_RTR_EOFB Op=Move El1=C_0 Exit=RA_LOAD_BYTE
RREDI BS_RTR_DONE3

/── BS_TOKEN_VALUE: strip "key=" prefix, recompute hash ───────
/If tokbuf contains '=': copy bytes after '=' to front, update RA_LOAD_TLEN,
/recompute RA_LOAD_HASH for the value portion.
/If no '=': return as-is (whole token is the value).
NOLINK
CLEAR BS_TOKEN_VALUE El1=RA_BS_TMP3
NOVA BS_TV_IDX_JMP2BS_TV_LO Op=Jump Exit=BS_TV_LOOP
NOLINK
JEQ BS_TV_LOOP El1=RA_BS_TMP3 El2=RA_LOAD_TLEN Exit=BS_TV_NOEQ
NOVA BS_TV_BADDR Op=Add El1=BS_TOKBUF_BASE El2=RA_BS_TMP3 Exit=RA_BS_TMP
NOVA BS_TV_BYTE Op=Read El1=RA_BS_TMP Exit=RA_BS_TMP
JEQ BS_TV_EQC El1=RA_BS_TMP El2=EQUALS Exit=BS_TV_FOUND
NOVA BS_TV_INC Op=Add El1=RA_BS_TMP3 El2=C_1 Exit=RA_BS_TMP3
NOVA BS_TV_JMP Op=Jump Exit=BS_TV_LOOP
NOLINK
RREDI BS_TV_NOEQ
NOLINK
NOVA BS_TV_FOUND Op=Add El1=RA_BS_TMP3 El2=C_1 Exit=RA_BS_TMP3
CLEAR BS_TV_VIDX El1=RA_BS_TMP2
NOVA BS_TV_VIDX_JMP2BS_TV_CL Op=Jump Exit=BS_TV_CLOOP
NOLINK
JEQ BS_TV_CLOOP El1=RA_BS_TMP3 El2=RA_LOAD_TLEN Exit=BS_TV_CDONE
/A plain NOVA here, not the CHAIN directly: the JEQ's generated _J lux takes
/its false branch by falling through, and a CHAIN body does not become that
/fall-through target -- BS_TV_CLOOP_J was left with next=0, whose physical
/neighbour is BS_TV_HLOOP_J in the hash section below. Control skipped the
/copy entirely, so RA_LOAD_TLEN was never updated and every `key=value`
/token came back unstripped. An ordinary instruction does receive the link,
/exactly as BS_TV_LOOP_J -> BS_TV_BADDR and BS_TV_EQC_J -> BS_TV_INC do.
NOVA BS_TV_CGO Op=Jump Exit=BS_TV_CSRC
NOLINK
NOVA BS_TV_CSRC Op=Add El1=BS_TOKBUF_BASE El2=RA_BS_TMP3 Exit=RA_BS_TMP
NOVA Op=Read El1=RA_BS_TMP Exit=RA_BS_TMP
NOVA Op=Add El1=BS_TOKBUF_BASE El2=RA_BS_TMP2 Exit=RA_BS_FLAG
NOVA Op=Write El1=RA_BS_FLAG El2=RA_BS_TMP
NOVA Op=Add El1=RA_BS_TMP3 El2=C_1 Exit=RA_BS_TMP3
NOVA Op=Add El1=RA_BS_TMP2 El2=C_1 Exit=RA_BS_TMP2
NOVA Op=Jump Exit=BS_TV_CLOOP

NOLINK
NOVA BS_TV_CDONE Op=Move El1=RA_BS_TMP2 Exit=RA_LOAD_TLEN
CLEAR BS_TV_HIDX El1=RA_BS_TMP2
NOVA BS_TV_HASH0 Op=Move El1=BS_HASH0 Exit=RA_LOAD_HASH
NOVA BS_TV_HASH0_JMP2BS_TV_HL Op=Jump Exit=BS_TV_HLOOP
NOLINK
JEQ BS_TV_HLOOP El1=RA_BS_TMP2 El2=RA_LOAD_TLEN Exit=BS_TV_HDONE
NOVA BS_TV_HBADDR Op=Add El1=BS_TOKBUF_BASE El2=RA_BS_TMP2 Exit=RA_BS_TMP
NOVA Op=Read El1=RA_BS_TMP Exit=RA_BS_TMP
HASH_STEP BS_TV_HM33 RA_LOAD_HASH RA_BS_TMP RA_BS_TMP3 RA_LOAD_HASH
NOVA Op=Add El1=RA_BS_TMP2 El2=C_1 Exit=RA_BS_TMP2
NOVA Op=Jump Exit=BS_TV_HLOOP

NOLINK
RREDI BS_TV_HDONE

/── BS_SCAN_EQ: scan tokbuf for '=' → RA_BS_FLAG ─────────────
/RA_BS_FLAG = 1 if '=' found, 0 otherwise. Does not modify tokbuf.
NOLINK
CLEAR BS_SCAN_EQ El1=RA_BS_TMP3
NOVA BS_SE2_START Op=Jump Exit=BS_SE2_LOOP
NOLINK
JEQ BS_SE2_LOOP El1=RA_BS_TMP3 El2=RA_LOAD_TLEN Exit=BS_SE2_NF
NOVA BS_SE2_ADDR Op=Add El1=BS_TOKBUF_BASE El2=RA_BS_TMP3 Exit=RA_BS_TMP
NOVA BS_SE2_BYTE Op=Read El1=RA_BS_TMP Exit=RA_BS_TMP
JEQ BS_SE2_EQC El1=RA_BS_TMP El2=EQUALS Exit=BS_SE2_FOUND
NOVA BS_SE2_INC Op=Add El1=RA_BS_TMP3 El2=C_1 Exit=RA_BS_TMP3
NOVA BS_SE2_JMP Op=Jump Exit=BS_SE2_LOOP
NOLINK
NOVA BS_SE2_FOUND Op=Move El1=C_1 Exit=RA_BS_FLAG
RREDI BS_SE2_FRRET
NOLINK
CLEAR BS_SE2_NF El1=RA_BS_FLAG
RREDI BS_SE2_NRRET

/── LOAD_ELEM: classify current tokbuf's key → SK_ELEM_KEY ──
//
IN: BS_READ_TOKEN already ran; tokbuf holds the raw token.
OUT: SK_ELEM_KEY = canonical key code (see below). Tokbuf ends up
holding the STRIPPED value if a key was found (whatever BS_TOKEN_VALUE
already leaves it as), or the original, untouched token if not.

  0 = no '=' found at all (positional token)
  otherwise = the key's numeric code (1..N), assigned by KEY_HTAB
  (AUTOGEN_KEY_TABLE, loader.py) in sorted-key order. The caller routes by
  code through KEY_DEST[code] -> &SK_ARG_<key>; no name comparison and no
  per-key KEY_CODE constant is involved any more. The vocabulary is found by
  scanning every .re file's own tokens (_discover_key_vocabulary), not a
  hand-typed list, so a key used anywhere in the corpus is automatically
  recognised here, and no caller depends on which number a key gets this run.

The one canonical key router, the one place a token's key gets
recognised. Replaces three independent hand-rolled byte trees that used
to each carry their own copy of this exact classification (LOAD_READ_NAME,
LOAD_NOVA_READ_ARGS, LOAD_READARG_KV) -- see docs/DECISIONS.md "Wave 2 is one pass" and
this session's own history: At/Next were simply missing from two of
those three copies, and later Count/Width/Value/Body/Rel were missing
from this one (it used to be hand-typed too), silently, both times for
the same reason -- a hand list has no way to notice it fell behind.
Resolving the value and deciding where it goes remains the caller's own
job (see LOAD_ELEM_RESOLVE's CALLER CONTRACT) -- different callers write
to genuinely different, permanent destinations (SK_NOVA_E1 vs a fixed
RA_MAx register), so that part cannot be unified without breaking
forward-reference backfill (see BUGS.md Pattern Z).
//
NOLINK
RVOCA LOAD_ELEM Body=BS_SCAN_EQ
JZ LOAD_ELK_EQCK El1=RA_BS_FLAG Exit=LOAD_ELK_NOKEY
/Recognition is one HT_LOOKUP into the pre-built KEY_HTAB (AUTOGEN_KEY_TABLE,
/loader.py), not a MATCH byte-tree. Hash the key portion [0, RA_BS_TMP3) on
/the raw (not yet stripped) buffer -- the bytes before '=', whose index
/BS_SCAN_EQ left in RA_BS_TMP3. The lookup must read the key bytes before any
/stripping: BS_TOKEN_VALUE (LOAD_ELK_FINALIZE) is destructive, copying the
/value portion over the key text in place, and runs exactly once afterward.
/Full-name compare is kept (HT_NAME_CMP), not hash-only: same by-construction
/identity the command/symbol tables use.
NOVA LOAD_ELK_NLEN Op=Move El1=RA_BS_TMP3 Exit=RA_HT_NLEN
NOVA LOAD_ELK_NAME Op=Move El1=BS_TOKBUF_BASE Exit=RA_HT_NAME
/djb2 over the key bytes [0, NLEN) into RA_HT_HASH (seed 5381; masked to low
/32 at the end, exactly as BS_LOOKUP_AT does before calling HT_LOOKUP).
NOVA LOAD_ELK_H0 Op=Move El1=BS_HASH0 Exit=RA_HT_HASH
CLEAR LOAD_ELK_HI El1=RA_BS_TMP2
NOVA LOAD_ELK_HJ0 Op=Jump Exit=LOAD_ELK_HLOOP
NOLINK
JEQ LOAD_ELK_HLOOP El1=RA_BS_TMP2 El2=RA_HT_NLEN Exit=LOAD_ELK_HDONE
NOVA LOAD_ELK_HADDR Op=Add El1=BS_TOKBUF_BASE El2=RA_BS_TMP2 Exit=RA_BS_TMP
NOVA LOAD_ELK_HREAD Op=Read El1=RA_BS_TMP Exit=RA_BS_TMP
HASH_STEP LOAD_ELK_H33 RA_HT_HASH RA_BS_TMP RA_BS_TMP3 RA_HT_HASH
NOVA LOAD_ELK_HINC Op=Add El1=RA_BS_TMP2 El2=C_1 Exit=RA_BS_TMP2
NOVA LOAD_ELK_HLJ Op=Jump Exit=LOAD_ELK_HLOOP
NOLINK
NOVA LOAD_ELK_HDONE Op=And El1=RA_HT_HASH El2=MASK_LOW32 Exit=RA_HT_HASH
NOVA LOAD_ELK_TB Op=Move El1=KEY_HT_BASE Exit=RA_HT_BASE
NOVA LOAD_ELK_TS Op=Move El1=KEY_HT_SIZE Exit=RA_HT_SIZE
NOVA LOAD_ELK_TM Op=Move El1=KEY_HT_MASK Exit=RA_HT_MASK
RVOCA LOAD_ELK_LK Body=HT_LOOKUP
/Miss (result 0) -> positional; hit -> code into SK_ELEM_KEY, then strip value
/once (BS_TOKEN_VALUE), regardless of which key matched -- as before.
JZ LOAD_ELK_RESCK El1=RA_HT_RESULT Exit=LOAD_ELK_NOKEY
NOVA LOAD_ELK_SET Op=Move El1=RA_HT_RESULT Exit=SK_ELEM_KEY
RVOCA LOAD_ELK_FINALIZE Body=BS_TOKEN_VALUE
RREDI LOAD_ELK_FRET
NOLINK
CLEAR LOAD_ELK_NOKEY El1=SK_ELEM_KEY
RREDI LOAD_ELK_NKRET

/── BS_PACK_TOKBUF: pack token bytes into dense string ─────────
/IN:  BS_TOKBUF_BASE (1 byte/lux), RA_LOAD_TLEN = byte count
/OUT: RA_BS_TMP2 = addr of first packed lux (stride=1, NUL-terminated)
/Clobbers: RA_BS_TMP, RA_BS_TMP3, RA_BS_PACK_WORD/SHIFT/SIDX/DST
NOLINK
NOVA BS_PACK_TOKBUF Op=Add El1=RA_LOAD_TLEN El2=C_7 Exit=RA_BS_TMP
NOVA BPT_LUCES2 Op=Div El1=RA_BS_TMP El2=C_8 Exit=RA_ALLOC_COUNT
NOVA BPT_LUCES3 Op=Add El1=RA_ALLOC_COUNT El2=C_2 Exit=RA_ALLOC_COUNT
RVOCA BPT_ALLOC Body=ALLOC
NOVA BPT_SAVEDS Op=Move El1=RA_ALLOC_RESULT Exit=RA_BS_TMP2
NOVA BPT_INITDST Op=Move El1=RA_ALLOC_RESULT Exit=RA_BS_PACK_DST
CLEAR BPT_IS El1=RA_BS_PACK_SIDX
CLEAR BPT_IW El1=RA_BS_PACK_WORD
CLEAR BPT_ISH El1=RA_BS_PACK_SHIFT
JEQ BPT_LOOP El1=RA_BS_PACK_SIDX El2=RA_LOAD_TLEN Exit=BPT_FLUSH
NOVA BPT_BADDR Op=Add El1=BS_TOKBUF_BASE El2=RA_BS_PACK_SIDX Exit=RA_BS_TMP
NOVA BPT_BLOAD Op=Read El1=RA_BS_TMP Exit=RA_BS_TMP3
NOVA BPT_SIDINC Op=Add El1=RA_BS_PACK_SIDX El2=C_1 Exit=RA_BS_PACK_SIDX
PACK_BYTE BPT_PACK RA_BS_PACK_WORD RA_BS_PACK_SHIFT RA_BS_TMP3 Full=BPT_FULL
NOVA BPT_LBJMP Op=Jump Exit=BPT_LOOP
NOLINK
NOVA BPT_FULL Op=Jump Exit=BPT_FLUSH_WORD
NOLINK
NOVA BPT_FLUSH_WORD Op=Write El1=RA_BS_PACK_DST El2=RA_BS_PACK_WORD
NOVA BPT_DSTINC Op=Add El1=RA_BS_PACK_DST El2=C_1 Exit=RA_BS_PACK_DST
CLEAR BPT_WCLR El1=RA_BS_PACK_WORD
CLEAR BPT_SCLR El1=RA_BS_PACK_SHIFT
NOVA BPT_FWJMP Op=Jump Exit=BPT_LOOP
NOLINK
NOVA BPT_FLUSH Op=Write El1=RA_BS_PACK_DST El2=RA_BS_PACK_WORD
NOVA BPT_NULLINC Op=Add El1=RA_BS_PACK_DST El2=C_1 Exit=RA_BS_PACK_DST
CLEAR BPT_NULL El1=RA_BS_TMP3
NOVA BPT_NULLW Op=Write El1=RA_BS_PACK_DST El2=RA_BS_TMP3
RREDI BPT_RRET

/── BS_UNPACK_TOKBUF: inverse of BS_PACK_TOKBUF ────────────────
/IN:  RA_BS_TMP2 = addr of first packed lux (from BS_PACK_TOKBUF),
/     RA_LOAD_TLEN = byte count to unpack
/OUT: BS_TOKBUF_BASE[0..RA_LOAD_TLEN-1] = the original bytes, 1/lux
/Clobbers: RA_BS_TMP, RA_BS_TMP3, RA_BS_PACK_WORD/SHIFT/SIDX/DST
NOLINK
CLEAR BS_UNPACK_TOKBUF El1=RA_BS_PACK_SIDX
JEQ UPT_LOOP El1=RA_BS_PACK_SIDX El2=RA_LOAD_TLEN Exit=UPT_RET
NOVA UPT_WIDX Op=Div El1=RA_BS_PACK_SIDX El2=C_8 Exit=RA_BS_TMP3
NOVA UPT_WADDR Op=Add El1=RA_BS_TMP2 El2=RA_BS_TMP3 Exit=RA_BS_PACK_DST
NOVA UPT_WLOAD Op=Read El1=RA_BS_PACK_DST Exit=RA_BS_PACK_WORD
NOVA UPT_BINW Op=Rem El1=RA_BS_PACK_SIDX El2=C_8 Exit=RA_BS_TMP
NOVA UPT_SHIFT Op=Mul El1=RA_BS_TMP El2=C_8 Exit=RA_BS_PACK_SHIFT
UNPACK_BYTE UPT_UB RA_BS_PACK_WORD RA_BS_PACK_SHIFT RA_BS_TMP
NOVA UPT_DADDR Op=Add El1=BS_TOKBUF_BASE El2=RA_BS_PACK_SIDX Exit=RA_BS_TMP3
NOVA UPT_DWRITE Op=Write El1=RA_BS_TMP3 El2=RA_BS_TMP
NOVA UPT_INC Op=Add El1=RA_BS_PACK_SIDX El2=C_1 Exit=RA_BS_PACK_SIDX
NOVA UPT_JMP Op=Jump Exit=UPT_LOOP
NOLINK
RREDI UPT_RET

/── BS_PARSE_INT: parse integer from tokbuf → RA_BS_PIVAL ──────
/Handles optional '-' prefix. Stops at first non-digit byte.
/IN:  BS_TOKBUF_BASE[0..RA_LOAD_TLEN-1] = decimal digits
/OUT: RA_BS_PIVAL = signed integer value (dedicated reg, no scratch collision)
NOLINK
CLEAR BS_PARSE_INT El1=RA_BS_PIVAL
CLEAR BS_PI_IDX El1=RA_BS_TMP3
CLEAR BS_PI_NEG El1=RA_BS_FLAG
CLEAR BS_PI_NEGF El1=RA_BS_TMP2
NOVA BS_PI_RB0 Op=Read El1=BS_TOKBUF_BASE Exit=RA_BS_TMP
JEQ BS_PI_DCHK El1=RA_BS_TMP El2=MINUS Exit=BS_PI_NEG_SET
NOVA BS_PI_START Op=Jump Exit=BS_PI_LOOP
NOLINK
NOVA BS_PI_NEG_SET Op=Move El1=C_1 Exit=RA_BS_TMP2
NOVA BS_PI_NIDX Op=Add El1=RA_BS_TMP3 El2=C_1 Exit=RA_BS_TMP3
NOVA BS_PI_LOOP2 Op=Jump Exit=BS_PI_LOOP
NOLINK
JEQ BS_PI_LOOP El1=RA_BS_TMP3 El2=RA_LOAD_TLEN Exit=BS_PI_DONE
NOVA BS_PI_BADDR Op=Add El1=BS_TOKBUF_BASE El2=RA_BS_TMP3 Exit=RA_BS_TMP
NOVA BS_PI_BYTE Op=Read El1=RA_BS_TMP Exit=RA_BS_TMP
NOVA BS_PI_DCK0 Op=ULess El1=RA_BS_TMP El2=ASCII_0 Exit=RA_BS_FLAG
NOVA BS_PI_DCK0J Op=JumpIf El1=RA_BS_FLAG Exit=BS_PI_DONE
NOVA BS_PI_DCK9 Op=ULess El1=ASCII_9 El2=RA_BS_TMP Exit=RA_BS_FLAG
NOVA BS_PI_DCK9J Op=JumpIf El1=RA_BS_FLAG Exit=BS_PI_DONE
NOVA BS_PI_SUB0 Op=Sub El1=RA_BS_TMP El2=ASCII_0 Exit=RA_BS_TMP
NOVA BS_PI_MUL Op=Mul El1=RA_BS_PIVAL El2=C_10 Exit=RA_BS_PIVAL
NOVA BS_PI_ADD Op=Add El1=RA_BS_PIVAL El2=RA_BS_TMP Exit=RA_BS_PIVAL
NOVA BS_PI_INC Op=Add El1=RA_BS_TMP3 El2=C_1 Exit=RA_BS_TMP3
NOVA BS_PI_JMP Op=Jump Exit=BS_PI_LOOP
NOLINK
NOVA BS_PI_DONE Op=JumpIf El1=RA_BS_TMP2 Exit=BS_PI_NEGATE
RREDI BS_PI_OKRET
NOLINK
NOVA BS_PI_NEGATE Op=Sub El1=C_0 El2=RA_BS_PIVAL Exit=RA_BS_PIVAL
RREDI BS_PI_NRRET
