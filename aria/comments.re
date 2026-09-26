//
comments.re — Comment delimiter declaration + stripping

TWO SEPARATE CONCERNS:

   1. DECLARATION — what the delimiter IS.
      COMMENT_OPEN / COMMENT_CLOSE below. Data, not behaviour.

   2. STRIPPING — removing comments from a byte stream.
      STRIP_COMMENTS below. The canonical implementation.

The Python bootstrap (loader.py::_strip_comments) is a DUPLICATE of
this aria. It exists only because bootstrap cannot run Reca before
Reca is loaded. The canonical behaviour lives here.

DO NOT let the two drift. Any change to one must be made to the other
and re-verified over every .re file, by diffing both implementations'
output line for line. That check is what found the four rules the old
inline reader was missing — each of which silently corrupted real
files. Nothing else caught them. See COMMENTS.md.

PROVIDES: COMMENT_OPEN, COMMENT_CLOSE, RA_STRIP_IN, RA_STRIP_OUT, STRIP_COMMENTS
DEPENDENCY: aspects.re
//


SET COMMENT_OPEN Value="//"

SET COMMENT_CLOSE Value="//"

//
THE RULE

A marker must be ALONE on its line, whitespace aside:

    (marker)
    prose, mentioning pairs freely
    (marker)

That is all of it. Consequences:

  - A pair anywhere else — in prose, in a quoted string, in a path, in
    an arrow like prev --Next--> curr — is just text. Nothing to
    disambiguate, so nothing to get wrong.
  - A line whose first non-whitespace byte is a lone slash is a
    whole-line comment. This is most comments in the project.
  - A trailing slash-comment on a code line is not stripped here at
    all, and never was: the dispatcher drops a line's tail once the
    command has read its arguments.

WHY IT IS THIS AND NOT THE OBVIOUS THING

The obvious rule — a pair anywhere opens or closes a block — needs
four interacting rules to be correct, and every one of them was a real
bug found the hard way:

  1. A pair only counts if the bytes on BOTH sides of it are not
     slashes. Miss the right side and ascii.re's own three-slash
     comment opens a block that eats every command up to the next
     unrelated pair. Miss the left and a four-slash run opens on its
     last pair.
  2. Whole-line slash comments must not be scanned, or any pair in
     prose false-opens a block.
  3. Closing a block must discard the rest of that line, not just the
     marker.
  4. A string literal must not span lines, while a block comment must.
     One unbalanced quote in a trailing comment otherwise swallows
     everything to the next quote, dozens of lines away.

Rule 3 is the tell: it exists only to contain the damage when a pair
closes where the author did not intend. Not derivable, only
memorisable. Under the lone rule, rules 1, 3 and 4 do not exist — not
"are simpler", do not exist — and the registers that implemented them
are gone with them.

STATE TRANSITIONS

   NORMAL + marker line   → BLOCK   (line dropped)
   NORMAL + lone-slash BOL→ NORMAL  (line dropped)
   NORMAL + other         → NORMAL  (line kept)
   BLOCK  + marker line   → NORMAL  (line dropped)
   BLOCK  + other         → BLOCK   (line dropped)
   Any    + NUL           → emit NUL, done
//

NEW RA_STRIP_IN
NEW RA_STRIP_OUT

SET SC_ST_NORMAL Value=0
SET SC_ST_BLOCK Value=1

//
STRIP_COMMENTS: line-based, lone-marker rule

IN:  RA_STRIP_IN  = addr of first byte of NUL-terminated input chain
     RA_STRIP_OUT = addr of first byte of NUL-terminated output chain
OUT: output chain filled, NUL-terminated.

Scans a line at a time. A line that is exactly a marker toggles block
state and is itself dropped. Inside a block, lines are dropped.
Outside, a line whose first non-whitespace byte is a slash is dropped
— which also covers a marker-with-text line, since it starts with a
slash. Everything else is copied through.

Dropping works by rewinding SL_OPTR back to SL_LSTART, the output
position where the current line began: bytes are emitted as they are
read, then un-emitted if the finished line turns out to be droppable.
No lookahead is needed anywhere.

SL_MARK counts the line's leading slashes; SL_TAIL marks that non-slash
content followed. A marker line is exactly SL_MARK == 2, SL_TAIL == 0.
//

NEW SL_STATE     /0=NORMAL 1=BLOCK
NEW SL_BYTE
NEW SL_IPTR
NEW SL_OPTR
NEW SL_LSTART    /output ptr at the current line's start (to rewind a dropped line)
NEW SL_MARK      /count of leading slashes on this line
NEW SL_TAIL      /1 once a non-slash, non-ws byte is seen on this line
NEW SL_SEEN      /1 once any non-ws byte is seen on this line
NEW SL_STR       /1 while inside a quoted string on this line
NEW SL_CUT       /1 once a trailing comment has started on this line
NEW SL_PREV      /previous byte, for the "slash follows whitespace" test
NEW SL_PEEKB     /lookahead byte, to tell a lone slash from a pair
NEW SL_TRIMP     /scratch address while trimming a line's trailing blanks

NOLINK
/Init
NOVA STRIP_COMMENTS Op=Move El1=SC_ST_NORMAL Exit=SL_STATE
NOVA SL_INIT_I Op=Move El1=RA_STRIP_IN Exit=SL_IPTR
NOVA SL_INIT_O Op=Move El1=RA_STRIP_OUT Exit=SL_OPTR
NOVA SL_INIT_LS Op=Move El1=RA_STRIP_OUT Exit=SL_LSTART
CLEAR SL_INIT_M El1=SL_MARK
CLEAR SL_INIT_T El1=SL_TAIL
CLEAR SL_INIT_S El1=SL_SEEN
CLEAR SL_INIT_C El1=SL_CUT
CLEAR SL_INIT_Q El1=SL_STR
CLEAR SL_INIT_P El1=SL_PREV

NOVA SL_LOOP Op=Read El1=SL_IPTR Exit=SL_BYTE
NOVA SL_INC Op=Add El1=SL_IPTR El2=C_1 Exit=SL_IPTR
/NUL: classify the final line, then done
JZ SL_NULCK El1=SL_BYTE Exit=SL_EOF
/LF ends the line
JEQ SL_LFCK El1=SL_BYTE El2=LF Exit=SL_EOL
//
Already cutting this line's trailing comment: swallow every byte until
LF. SL_PREV is still tracked so the next line starts clean.
//
JZ SL_CUTCK El1=SL_CUT Exit=SL_NOTCUT
NOVA SL_CUT_P Op=Move El1=SL_BYTE Exit=SL_PREV
NOVA SL_CUT_J Op=Jump Exit=SL_LOOP
NOLINK
/Leading whitespace, before any content on this line
JZ SL_NOTCUT El1=SL_SEEN Exit=SL_WSCK
NOVA SL_SEENJ Op=Jump Exit=SL_BODY
NOLINK
JEQ SL_WSCK El1=SL_BYTE El2=SP Exit=SL_EMIT
JEQ SL_WSTAB El1=SL_BYTE El2=TAB Exit=SL_EMIT
/First content byte of this line
NOVA SL_MARKSEEN Op=Move El1=C_1 Exit=SL_SEEN
//
Body. Two questions, in this order:

  1. Quotes. Inside a string, nothing is a comment -- that is what keeps
     SET COMMENT_OPEN "//" and SET PATH "/aria/io.re" intact.
  2. A lone slash following whitespace starts a trailing comment.
     "Lone" matters: /// stays inert. "Following whitespace" matters:
     it is what separates a comment from a slash inside a token.

Everything else just counts leading slashes for the marker test.
//
JZ SL_BODY El1=SL_STR Exit=SL_NOTSTR
/Inside a string: emit, and close it on the next quote
JEQ SL_STR_QCK El1=SL_BYTE El2=DQUOTE Exit=SL_STR_CLOSE
NOVA SL_STR_PJ Op=Move El1=SL_BYTE Exit=SL_PREV
NOVA SL_STR_J Op=Jump Exit=SL_EMIT
NOLINK
NOVA SL_STR_CLOSE Op=Move El1=C_0 Exit=SL_STR
NOVA SL_STR_CP Op=Move El1=SL_BYTE Exit=SL_PREV
NOVA SL_STR_CJ Op=Jump Exit=SL_EMIT
NOLINK
/Not in a string. Opening quote?
JEQ SL_NOTSTR El1=SL_BYTE El2=DQUOTE Exit=SL_STR_OPEN
NOVA SL_NOTSTR_J Op=Jump Exit=SL_SLASHCK
NOLINK
NOVA SL_STR_OPEN Op=Move El1=C_1 Exit=SL_STR
NOVA SL_STR_OP Op=Move El1=SL_BYTE Exit=SL_PREV
NOVA SL_STR_OJ Op=Jump Exit=SL_EMIT
NOLINK
/Slash?
JEQ SL_SLASHCK El1=SL_BYTE El2=SLASH Exit=SL_SLASH
NOVA SL_TAILSET Op=Move El1=C_1 Exit=SL_TAIL
NOVA SL_TAILP Op=Move El1=SL_BYTE Exit=SL_PREV
NOVA SL_TAILJ Op=Jump Exit=SL_EMIT
NOLINK
//
A slash. If the previous byte was whitespace and the next is not a
slash, this begins a trailing comment: stop emitting for this line.
Otherwise it is ordinary content -- count it toward the marker test if
nothing else has been seen on the line yet.
//
JEQ SL_SLASH El1=SL_PREV El2=SP Exit=SL_SL_WSCK
JEQ SL_SL_TABCK El1=SL_PREV El2=TAB Exit=SL_SL_WSCK
NOVA SL_SL_ORD Op=Jump Exit=SL_SL_COUNT
NOLINK
/Preceded by whitespace: peek ahead for a second slash
NOVA SL_SL_WSCK Op=Read El1=SL_IPTR Exit=SL_PEEKB
JEQ SL_SL_PK2 El1=SL_PEEKB El2=SLASH Exit=SL_SL_COUNT
/Lone slash after whitespace: cut the rest of the line
NOVA SL_SL_DOCUT Op=Move El1=C_1 Exit=SL_CUT
NOVA SL_SL_CUTP Op=Move El1=SL_BYTE Exit=SL_PREV
NOVA SL_SL_CUTJ Op=Jump Exit=SL_LOOP
NOLINK
/Ordinary slash: only counts as leading while no tail has been seen
JZ SL_SL_COUNT El1=SL_TAIL Exit=SL_MARKINC
NOVA SL_SLJ Op=Jump Exit=SL_SL_EMITP
NOLINK
NOVA SL_MARKINC Op=Add El1=SL_MARK El2=C_1 Exit=SL_MARK
/Explicit jump to SL_SL_EMITP -- this used to fall through by address
/adjacency (no NOLINK broke it, but nothing pointed here either), which
/happened to reach SL_SL_EMITP under the old allocation order. Address
/reshuffling elsewhere (e.g. the Wave 2 single-pass merge) put the JZ
/macro's own auto-generated JumpIf step (SL_SL_COUNT_J) immediately
/after this instead, re-testing SL_TAIL (still 0) and jumping straight
/back to SL_MARKINC -- an infinite loop, confirmed directly via a
/patched single-execution trace of freeze()'s own LOAD_MAIN run
/(self-hosting stuck at 1/42, spinning here for the full step budget).
/Same class as LOAD_MAIN_SEAL_W's fix this session -- see BUGS.md
/Pattern Z.
NOVA SL_MARKINCJ Op=Jump Exit=SL_SL_EMITP
NOLINK
NOVA SL_SL_EMITP Op=Move El1=SL_BYTE Exit=SL_PREV
NOVA SL_MARKJ Op=Jump Exit=SL_EMIT
NOLINK
NOVA SL_EMIT Op=Write El1=SL_OPTR El2=SL_BYTE
NOVA Op=Add El1=SL_OPTR El2=C_1 Exit=SL_OPTR
NOVA Op=Jump Exit=SL_LOOP

//
── End of line: classify it ─────────────────────────────────
Marker line (two leading slashes, no tail) toggles state and drops.
A line inside a block drops. A line starting with any slash drops.
Anything else stays.
//
NOLINK
//
Trim this line's trailing blanks before the LF goes down.

Cutting a trailing comment leaves the whitespace that separated it from
the code behind, so "NEW Name=Y  /note" ended as "NEW Name=Y  ".
loader.py's stripper rstrips each line, this did not -- the two produced
different bytes for the same source line. Nothing downstream reads
trailing blanks, so it changed no behaviour, but a stripper whose whole
job is to mirror the reference implementation should not disagree with
it at all.

The floor is SL_LSTART, so a line that is nothing but blanks trims to
empty and never walks back into the previous line's bytes.
//
JEQ SL_EOL El1=SL_OPTR El2=SL_LSTART Exit=SL_EOL_W
NOVA SL_EOL_TRIMA Op=Sub El1=SL_OPTR El2=C_1 Exit=SL_TRIMP
NOVA SL_EOL_TRIMR Op=Read El1=SL_TRIMP Exit=SL_PEEKB
JEQ SL_EOL_TRIMS El1=SL_PEEKB El2=SP Exit=SL_EOL_TRIMD
JEQ SL_EOL_TRIMT El1=SL_PEEKB El2=TAB Exit=SL_EOL_TRIMD
NOVA SL_EOL_TRIMX Op=Jump Exit=SL_EOL_W
NOLINK
NOVA SL_EOL_TRIMD Op=Move El1=SL_TRIMP Exit=SL_OPTR
NOVA SL_EOL_TRIMJ Op=Jump Exit=SL_EOL
NOLINK
NOVA SL_EOL_W Op=Write El1=SL_OPTR El2=SL_BYTE
NOVA SL_EOLI Op=Add El1=SL_OPTR El2=C_1 Exit=SL_OPTR
JZ SL_EOL_TCK El1=SL_TAIL Exit=SL_EOL_MCK
NOVA SL_EOL_TJ Op=Jump Exit=SL_EOL_KEEPCK
NOLINK
JEQ SL_EOL_MCK El1=SL_MARK El2=C_2 Exit=SL_EOL_TOGGLE
NOVA SL_EOL_MJ Op=Jump Exit=SL_EOL_KEEPCK
NOLINK
/Marker line: toggle, then drop it
JZ SL_EOL_TOGGLE El1=SL_STATE Exit=SL_EOL_ENTER
NOVA SL_EOL_LEAVE Op=Move El1=SC_ST_NORMAL Exit=SL_STATE
NOVA SL_EOL_LEAVEJ Op=Jump Exit=SL_EOL_DROP
NOLINK
NOVA SL_EOL_ENTER Op=Move El1=SC_ST_BLOCK Exit=SL_STATE
NOVA SL_EOL_ENTERJ Op=Jump Exit=SL_EOL_DROP
NOLINK
/Not a marker: drop if in a block, or if the line starts with a slash
JZ SL_EOL_KEEPCK El1=SL_STATE Exit=SL_EOL_SLCK
NOVA SL_EOL_INBLK Op=Jump Exit=SL_EOL_DROP
NOLINK
JZ SL_EOL_SLCK El1=SL_MARK Exit=SL_EOL_KEEP
NOVA SL_EOL_SLJ Op=Jump Exit=SL_EOL_DROP
NOLINK
/Keep: this line's bytes stay, next line starts here
NOVA SL_EOL_KEEP Op=Move El1=SL_OPTR Exit=SL_LSTART
NOVA SL_EOL_KEEPJ Op=Jump Exit=SL_EOL_RESET
NOLINK
//
Drop: rewind output to where this line began, then emit a bare LF in
its place. The line's content goes; the line itself stays.

This matters more than it looks. loader.py's stripper returns a blank
entry for every dropped line rather than removing it, so line numbers
and blank-line boundaries survive stripping. The loader leans on both:
CHAIN, SWITCH, FOR and NOVAE bodies are delimited by indentation, and
blank lines reset that context. Collapsing dropped lines away shifts
every following line up, silently reshaping those blocks -- which took
the self-hosted build from 22/42 to 1/42 when this was missing.
//
NOVA SL_EOL_DROP Op=Move El1=SL_LSTART Exit=SL_OPTR
NOVA Op=Write El1=SL_OPTR El2=LF
NOVA Op=Add El1=SL_OPTR El2=C_1 Exit=SL_OPTR
NOVA SL_EOL_DLS Op=Move El1=SL_OPTR Exit=SL_LSTART
NOVA SL_EOL_DLSJ Op=Jump Exit=SL_EOL_RESET

/SL_EOL_DLSJ is not decoration. This block used to end at SL_EOL_DLS and
/reach the per-line reset below by physical adjacency alone; allocation
/order put SL_EOL_RESET (the lux that clears SL_MARK) BEFORE this block,
/so the fall-through landed on SL_EOL_RT and skipped the SL_MARK reset
/entirely. Every dropped line therefore left SL_MARK non-zero forever,
/which made SL_EOL_SLCK treat every following line as a slash line and
/drop it too -- one comment line at the top of a file silently reduced
/that whole file to bare newlines. Same class as SL_MARKINC's fix above.
/See BUGS.md Pattern Z.
NOLINK
NOVA SL_EOL_RESET Op=Move El1=C_0 Exit=SL_MARK
NOVA SL_EOL_RT Op=Move El1=C_0 Exit=SL_TAIL
NOVA Op=Move El1=C_0 Exit=SL_SEEN
NOVA Op=Move El1=C_0 Exit=SL_CUT
NOVA Op=Move El1=C_0 Exit=SL_STR
NOVA Op=Move El1=C_0 Exit=SL_PREV
NOVA Op=Jump Exit=SL_LOOP

//
── EOF: classify the final, unterminated line, then finish ──
//
NOLINK
JZ SL_EOF El1=SL_STATE Exit=SL_EOF_SLCK
NOVA SL_EOF_INBLK Op=Move El1=SL_LSTART Exit=SL_OPTR
NOVA SL_EOF_INBJ Op=Jump Exit=SL_DONE
NOLINK
JZ SL_EOF_SLCK El1=SL_MARK Exit=SL_DONE
NOVA SL_EOF_DROP Op=Move El1=SL_LSTART Exit=SL_OPTR
NOVA SL_DONE Op=Write El1=SL_OPTR El2=C_0
RREDI SL_RRET
