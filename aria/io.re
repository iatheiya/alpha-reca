//
aria/io.re — File I/O constants

Standard file descriptors, open flags, permission masks, AT_FDCWD.
Exire numbers live in runtime/registers.re — not redeclared here.

DEPENDENCY: aspects.re
//

── Standard file descriptors ─────────────────────────────────
SET STDIN Value=0

SET STDOUT Value=1

SET STDERR Value=2

── Open flags (ARM64 Linux) ──────────────────────────────────
SET O_RDONLY Value=0

SET O_WRONLY Value=1

SET O_RDWR Value=2

SET O_CREAT Value=64 /0100 octal

SET O_TRUNC Value=512 /01000 octal

SET O_APPEND Value=1024 /02000 octal

── File permission masks ─────────────────────────────────────
SET PERM_RW_R_R Value=420 /0644: owner rw, group r, others r

SET PERM_RWX_R_R Value=493 /0755: owner rwx, group rx, others rx

/── AT_FDCWD ─────────────────────────────────────────────────
//
openat(AT_FDCWD, path, flags, mode) == open(path, flags, mode)
-100 as uint64:
//
SET AT_FDCWD Value=18446744073709551516

//
── Read buffer ──────────────────────────────────────────────
Page-sized buffer for file I/O. Refilled by BS_READ_BYTE via SYS_READ.
Moved here from lexer.re: reading bytes off a disk is I/O, not
tokenisation. lexer.re owns turning a byte stream into tokens; it does
not own where the bytes come from.

RA_LOAD_RPOS, RA_LOAD_RLEN, RA_LOAD_FD are declared in registers.re,
which loads after this file -- resolved by the loader's own backfill,
same as every other forward reference here.
//
SET BS_READBUF_SIZE Value=4096
NEW BS_READBUF_000  Count=4096
NEW    BS_READBUF_BASE

/── BS_READBUF_INIT: initialize this file's own base pointer ───
/Must be called once before BS_READ_BYTE. Reads BS_READBUF_000's own
/self-ref word to get its lux address. Was part of a single, three-
/subsystem BS_INIT (htable/intern.re, tokbuf/lexer.re, readbuf/here)
/-- split so each file initializes only its own base pointer. Called
/by LOAD_MAIN (wax.re) alongside BS_HT_INIT (intern.re) and
/BS_TOKBUF_INIT (lexer.re).
NOVA BS_READBUF_INIT Op=Move El1=BS_READBUF_000 Exit=BS_READBUF_BASE

RREDI BS_READBUF_INIT_RET

//
── BS_READ_BYTE: read next byte → RA_LOAD_BYTE (0 on EOF) ───
Refills the read buffer via SYS_READ when RA_LOAD_RPOS == RA_LOAD_RLEN.

Redirect mode (RA_REDIRECT_BASE non-zero) overrides the file entirely
and serves bytes from an in-Aether buffer instead. Every caller
(BS_READ_TOKEN and everything above it) keeps working unchanged --
this is the seam tide will use to feed already-stripped text through
without any caller knowing the source changed.
//
NOLINK
JZ BS_READ_BYTE El1=RA_REDIRECT_BASE Exit=BS_RB_LIVE_CHECK
/Redirect mode active: read from the already-constructed buffer instead
/of the live file stream. RA_LOAD_RPOS/RA_LOAD_RLEN are left untouched.
JEQ BS_RB_RDEOF El1=RA_REDIRECT_POS El2=RA_REDIRECT_LEN Exit=BS_RB_RDDONE
NOVA BS_RB_RDADDR Op=Add El1=RA_REDIRECT_BASE El2=RA_REDIRECT_POS Exit=RA_BS_TMP
NOVA BS_RB_RDLOAD Op=Read El1=RA_BS_TMP Exit=RA_LOAD_BYTE
NOVA BS_RB_RDINC Op=Add El1=RA_REDIRECT_POS El2=C_1 Exit=RA_REDIRECT_POS
RREDI BS_RB_RDRET
NOLINK
CLEAR BS_RB_RDDONE El1=RA_LOAD_BYTE
RREDI BS_RB_RDDRET
NOLINK
JEQ BS_RB_LIVE_CHECK El1=RA_LOAD_RPOS El2=RA_LOAD_RLEN Exit=BS_RB_FILL
NOVA BS_RB_BADDR Op=Add El1=BS_READBUF_BASE El2=RA_LOAD_RPOS Exit=RA_BS_TMP
NOVA BS_RB_LOAD Op=Read El1=RA_BS_TMP Exit=RA_LOAD_BYTE
NOVA BS_RB_INC Op=Add El1=RA_LOAD_RPOS El2=C_1 Exit=RA_LOAD_RPOS
RREDI BS_RB_RET
NOVA BS_RB_FILL Op=Move El1=SYS_READ Exit=SC_NR
NOVA Op=Move El1=RA_LOAD_FD Exit=SC_A0
NOVA Op=Move El1=BS_READBUF_BASE Exit=SC_A1
NOVA Op=Move El1=BS_READBUF_SIZE Exit=SC_A2
NOVA Op=Exire El1=C_0 El2=C_0 Exit=C_0
NOVA Op=Move El1=SC_A0 Exit=RA_LOAD_RLEN

CLEAR BS_RB_POS El1=RA_LOAD_RPOS
JZ BS_RB_EOFCK El1=RA_LOAD_RLEN Exit=BS_RB_EOF
NOVA BS_RB_RETRY Op=Jump Exit=BS_READ_BYTE
NOLINK
CLEAR BS_RB_EOF El1=RA_LOAD_BYTE
RREDI BS_RB_EOFRET

//
── BS_PEEK_BYTE: look at the next byte without consuming it ──
Calls BS_READ_BYTE the ordinary way (works in both redirect and live
modes, refill included), then rewinds whichever position counter just
moved, so the very next real BS_READ_BYTE call sees the same byte
again. Skipped on EOF (RA_LOAD_BYTE==0) -- nothing advanced there, so
there is nothing to rewind, and decrementing anyway would walk the
position backward into whatever the buffer held before.

Added for SET's own value-start decision (quote vs digit): there was
no way to un-read a byte otherwise, project-wide, and the "read one
token, guess from its first byte" approach a token-based tokenizer
tempts you toward is unsafe here -- a quoted string can contain
whitespace, so the token that lands in the buffer may not be the whole
value. See BUGS.md/HISTORY.md for the session that needed this.
//
NOLINK
RVOCA BS_PEEK_BYTE Body=BS_READ_BYTE
JZ BS_PEEK_EOFCK El1=RA_LOAD_BYTE Exit=BS_PEEK_RET
JZ BS_PEEK_MODECK El1=RA_REDIRECT_BASE Exit=BS_PEEK_LIVE
NOVA BS_PEEK_RDEC Op=Sub El1=RA_REDIRECT_POS El2=C_1 Exit=RA_REDIRECT_POS
NOVA BS_PEEK_RJMP Op=Jump Exit=BS_PEEK_RET
NOLINK
NOVA BS_PEEK_LIVE Op=Sub El1=RA_LOAD_RPOS El2=C_1 Exit=RA_LOAD_RPOS
RREDI BS_PEEK_RET
