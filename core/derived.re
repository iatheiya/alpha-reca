//derived.re — Operations derived from the 24 native Aspects

 Every operation here is expressible from native Aspects in 1–3 steps.
 They exist for expressiveness, readability, and compiler Rule_* support.

 DEPENDENCY: aspects.re, core/constants.re
//

// ── Derived bitwise ──────────────────────────────────────────────────────────
 Not(a) = Xor(a, -1)
 Compiler Rule: xor i64 v1, -1
//

NEWREF Not

// ── Derived comparisons ──────────────────────────────────────────────────────
 Greater(a, b) = Less(b, a)  — element swap, no new CPU instruction needed.
 On most CPUs there is no native "greater than" — only "less than" and "equal".
 sgt (signed greater than) is itself a pseudo-instruction on most architectures;
 the assembler emits slt with swapped els.
 Compiler Rule: icmp slt v2, v1  (v2 and v1 swapped relative to Less)

 UGreater(a, b) = icmp ugt a, b
 NotEqual(a, b) = icmp ne a, b
 LessOrEqual(a, b) = icmp sle a, b
 ULessOrEqual(a, b) = icmp ule a, b
 GreaterOrEqual(a, b) = icmp sge a, b
 UGreaterOrEqual(a, b) = icmp uge a, b
 All produce i1 result (HasCmpResult) — fuse with following JumpIf via JumpIfCmp.
//

NEWREF Greater
NEWREF UGreater
NEWREF NotEqual
NEWREF LessOrEqual
NEWREF ULessOrEqual
NEWREF GreaterOrEqual
NEWREF UGreaterOrEqual

// ── Unconditional jump ───────────────────────────────────────────────────────
 Jump(exit) = JumpIf(C_1, exit)  — C_1 is always nonzero, branch always taken.
 Compiler emits: br label {label_dest}
//

NEWREF Jump

// ── Aether convenience ───────────────────────────────────────────────────────
 Move(src, exit) = exit.word <- src.word
 Load(src, exit) = exit.word <- Aether[src.word]   (same as Read)
 Store(src, val) = Aether[src.word] <- val.word    (same as Write)
//

NEWREF Move
NEWREF Load
NEWREF Store


