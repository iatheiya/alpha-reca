//
relations.re — Bootstrap interpreter conventions

These are NOT hardware aspects. They are aria conventions used by:
- loader.py (to wire instruction Lux with Op/El1/... lumina)
- interpreter.py (to dispatch and read operands)
- yaku.re (to scan Lumen and emit LLVM IR)
- parser.re (to build Lux at runtime)

A different aria may define different conventions without touching aspects.re.
These names exist in the symbol table so Python can find them by name.

The SEAL invariant applies: each relation's word = its own address.
This enables: Equal(lumen_rel, El1) where El1.word = addr(El1).

DEPENDENCY: aspects.re
NOTE: load order does not matter — Wave-A/B resolves all symbols before wiring.

── Instruction wiring relations ─────────────────────────────────────────────

Used by loader.py to add lumina to instruction Lux:
instr --Op-->          aspect_lux
instr --El1-->        operand_lux
instr --El2-->        operand_lux
instr --Exit-->       exit_lux
instr --Next/AutoNext--> next_instruction_lux
lux   --Entry-->  marker_lux  (marks function entry points)
//

SET Op

SET Next

SET El1

SET El2

SET Exit

SET Entry

── Compiler entry-point marker ───────────────────────────────────────────────

//
Yaku is NOT a runtime operation. It marks compiler entry points:
lux --Entry--> Yaku   means "lux is a function entry"
Used by yaku.re (P0_SCAN) and repl.py (_find_entry).
SelfYaku (defined in yaku.re) is a separate marker to distinguish
the compiler's own entry from user entry points during self-compile.
//

SET Yaku

── Compiler annotation relations ─────────────────────────────────────────────

//
Constant: marks a Lux created by SET as a compile-time constant.
lux --Constant--> Yaku  means the lux holds a fixed value the compiler can inline.
Attached by SET's handler to tag constants; read by yaku.re PRELOAD_CONST.
//

SET Constant

── LOCK's authorized writer ──────────────────────────────────────────────────

//
Key: the one exception to LOCK's write guard (see loader.py's
_combo_key/_lock_names, interpreter.py's _check_lock_write). Identical
write semantics to Write (aether[a1.word] = a2.word) -- the guard every
ordinary opcode enforces against a LOCK-protected target simply isn't
checked for Key. Being dispatched via Key, not Write, at the one
specific declared call site that legitimately needs to write to
LOCK-protected memory, IS the authorization: no separate authorized-code
region, no per-instruction flag, nothing else to check. Purely a Reca
interpreter convention -- no CPU has a "write, but exempt from this
project's own runtime guard" instruction, and it isn't expressible from
hardware ops either (Write itself already is hardware-native); it exists
only so Python can dispatch on it by name, exactly this file's own
purpose.
//

SET Key In=SAVE

//
Op1 on a Key line (`NOVA Op=Key Op1=Move El1=... Exit=...`) picks Move's
write shape (single indirection via Exit) instead of Key's own default,
Write's shape (double indirection via El1) -- for LOCK-protected data that
isn't shaped like the K_ double-deref pointer/value pattern, where Write's
shape wouldn't address the right cell at all. Op1 is a genuine extra-lumen
relation on the instruction (interpreter.py's _resolve_key_shape reads it
back at runtime, memoized per-pc once resolved, in interpreter.py's
_exec_key -- not a second op name: every Key instruction's own SLOT_OP
stays literally Key regardless of Op1, so there is nothing to declare
here). Plain `Op=Key` (no Op1) is unaffected, still Write's shape.
//
