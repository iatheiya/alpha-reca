# Comments in Reca

## The rule

A block comment marker must be **alone on its line**, whitespace aside:

```
//
prose, freely mentioning // and "//" and /aria/paths
//
```

That is the whole rule. Three consequences, all of them the point:

- A pair appearing anywhere else — in prose, in a quoted string, in a
  path, in a `--Next-->` arrow — is **just text**. Nothing to
  disambiguate, so nothing to get wrong.
- A line whose first non-whitespace character is a lone `/` is a
  whole-line comment. Unchanged; this is most comments in the project.
- A trailing `/comment` on a code line is not stripped at all. It never
  was. The dispatcher drops a line's tail once the command has read its
  arguments.

Indentation is fine — a marker inside an indented `SAVE`/`CHAIN` body
works the same.

## Why it changed

The old rule was: a pair anywhere outside quotes opens or closes a
block. Sounds simple. It is not. Making it correct required four
interacting rules, and **every one of them was a real bug found the
hard way**:

1. **Both-sided slash-run check.** A pair only counts if the byte
   before it *and* the byte after it are not slashes. Missing the
   right-hand check meant `SET SLASH 47   ///` (ascii.re's own comment
   about the slash constant) opened a block that ate every command up
   to the next unrelated pair, anywhere later in the file. Missing the
   left-hand check meant `////` opened on its last pair.

2. **Whole-line `/` comments are not scanned.** Otherwise any pair in
   prose false-opened a block.

3. **Closing a block discards the rest of that line.** Not just the
   marker — the tail too.

4. **A string literal does not span lines; a block comment does.** One
   unbalanced quote inside a trailing comment (parser.re had one)
   otherwise swallowed everything to the next quote, dozens of lines
   away.

Rule 3 is the tell. It exists only to contain the damage when a pair
closes somewhere the author didn't intend. It is not derivable from
anything; you either memorise it or you are surprised by it. That is a
sign the rule underneath is wrong.

Under the lone-marker rule, **rules 1, 3 and 4 do not exist**. Not
"are simpler" — do not exist. There is nothing to disambiguate, so the
registers that did the disambiguating (`SC_PREV`, `SC_PREV2`,
`SC_PEEK`, `SC_BOL`) are gone too: eight registers down to three.

There is also a readability argument, and it is not decorative. Under
the old rule you had to look at the *end* of a line to know whether a
comment was still open. While writing this very file, that mistake was
made three times in a row — prose mentioning `//` silently closed its
own block and dumped 85 lines of English into the output as code.

## What comments are not

They are not data. There is no way to read a comment's text back from
a running program, and there should not be.

An earlier attempt at the opposite — `inscription.re`, an `INSCRIPTION`
relation plus a `SCAN_INSCRIPTIONS` walker, meant to materialise free
text as readable luces — has been **deleted**, along with its Python
half (`_record_inscription`, `_finalize_inscriptions`). Both sides were
dead code: `INSCRIPTION` was referenced nowhere outside its own
declaration, and `_record_inscription` was never called. The mechanism
was built and never wired up, in either language, for the whole life of
the project.

The reason it was never wanted is worth stating, because it will come
up again: **the moment something reads a comment's text, that text is
data, not a comment.** And Reca already has a way to declare data — a
lux, with `NEW` + `SET`, linked where it's needed. "Mark it as a
comment but let it be read" is just that, with extra steps and an extra
state that behaves like data everywhere it is actually used.

So: if text should be readable, declare it. If it shouldn't, comment
it, and it is gone at load time. There is no third thing.

## Where the rule lives

One place: `comments.re`'s `STRIP_COMMENTS`, called once per file by
`wax.re` before any wave sees a byte. `loader.py`'s
`_strip_comments` is a duplicate of it and exists only because
bootstrap cannot run Reca before Reca is loaded. The canonical
behaviour is the aria's.

**Do not let the two drift.** Any change to one must be made to the
other and re-verified over every `.re` file, by diffing both
implementations' output line for line. That check is what found the
four rules the old inline reader was missing — each of which silently
corrupted real files. Nothing else caught them.

Five other copies of comment detection used to exist, live and
mid-parse, scattered through the loader:

1. `BS_READ_TOKEN`'s whitespace skip,
2. `BS_READ_TOKEN`'s token-collection loop,
3. `BS_SKIP_TO_EOL_CW`, added as a stopgap when the second copy proved
   unable to see a marker later on an unrecognised line,
4. `LOAD_PREPASS_FILE`'s own private machine,
5. `LOAD_DL_COLLECT`'s own slash branch.

All five are deleted. `BS_BLOCK_SKIP` is gone with them. A tokenizer
has no business knowing what a comment is, and now it doesn't: by the
time any byte reaches it, tide has already removed them.

If a comment ever reaches the tokenizer again, the bug is in tide, not
there. Do not re-add a check.

## The migration

604 lines across 33 files were written in the old inline style
(`//text`, `text//`, `//text//`). They were rewritten mechanically by
`migrate_comments.py`.

The script does not regex for `//`. It walks each line byte by byte
carrying the same state the old reader carries (in a block? in a
string? part of a slash run?) and only moves what that reader would
have called a marker. Everything else it leaves alone — which is why
`SET COMMENT_OPEN "//"`, `prev --Next--> curr`, `///` and `////` came
through untouched.

Its safety property is checked per file, and it is the important part:
**the surviving lines under the old rule, before rewriting, must equal
the surviving lines under the new rule, after rewriting.** A file that
fails is left alone and reported. None failed.

Result: 34 files rewritten, 4 already clean, and all 38 now produce
identical output under both rules — which is what made flipping the
default safe.

Three inline pairs remain project-wide, all inert under both rules and
all correct as they are: `ascii.re`'s `SET SLASH 47   ///`, and two
whole-line `/` comments whose text happens to mention a pair.

## Status

Done. The lone rule is the only rule, and it runs in exactly one place.

- `comments.re` holds one machine, `STRIP_COMMENTS`. It went from 494
  lines to 221, and from eight registers to three: `SC_PREV`,
  `SC_PREV2`, `SC_PEEK` and `SC_BOL` existed only to tell a real
  marker from an accidental one, and there is no longer any such
  question.
- `loader.py` holds one implementation. The `COMMENT_MODE` switch, the
  second implementation, and the `K_COMMENT_LONE` flag that mirrored
  the mode into the Aether are all removed.
- `wax.re` calls `STRIP_COMMENTS` once per file, before any wave, and
  points the byte source at the cleaned text via the redirect seam that
  already existed for CHAIN/FOR/SAVE bodies. Nothing downstream
  changed: `BS_READ_TOKEN` and the dispatcher read from the same
  `BS_READ_BYTE` as always, unaware the source moved.
- All five live comment checks in the loader are deleted, along with
  `BS_BLOCK_SKIP` and `migrate_comments.py`.

One bug found while wiring tide is worth remembering, because it will
recur for anything else built on `SPAN`: a `SPAN` region's `SEAL`
self-ref word sits at `base+0`, *inside* the data. The first byte
written destroys it. `TIDE_INIT` therefore resolves the base pointers
**once**, guarded by `TIDE_READY` — reading the self-ref again on the
second file yielded 47 (an ASCII slash, the first character of the
file) instead of an address, and everything after wrote into low
memory. `BS_INIT` has the identical layout and survives for the same
reason: it runs exactly once. This took self-hosting from 22/42 to
1/42 and back.

Self-hosting is 19/42. It was 22/42 before tide; the three-file
difference and the stall in `lexer.re` are not yet explained.
