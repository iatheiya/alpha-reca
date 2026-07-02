//relations.re — Bootstrap interpreter conventions

These are NOT hardware aspects. They are aria conventions used by:
- loader.py (to wire instruction Lux with Op/El1/... lumina)
- interpreter.py (to dispatch and read operands)
- yaku.re (to scan Lumen and emit LLVM IR)
- parser.re (to build Lux at runtime)

A different aria may define different conventions without touching aspects.re.
These names exist in the symbol table so Python can find them by name.

"SETREF" invariant applies: each relation's word = its own address.
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
lux   --Entry-->  marker_lux  (marks function entry points)//

NEWREF Op

NEWREF Next

NEWREF El1

NEWREF El2

NEWREF Exit

NEWREF Entry

── Compiler entry-point marker ───────────────────────────────────────────────

//Yaku is NOT a runtime operation. It marks compiler entry points:
lux --Entry--> Yaku   means "lux is a function entry"
Used by yaku.re (P0_SCAN) and repl.py (_find_entry).
SelfYaku (defined in yaku.re) is a separate marker to distinguish
the compiler's own entry from user entry points during self-compile.//

NEWREF Yaku

── Compiler annotation relations ─────────────────────────────────────────────

//Constant: marks a Lux created by NEWSET as a compile-time constant.
lux --Constant--> Yaku  means the lux holds a fixed value the compiler can inline.
Used by LOAD_CMD_NEWSET (saku.re) to tag constants; read by yaku.re PRELOAD_CONST.//

NEWREF Constant
