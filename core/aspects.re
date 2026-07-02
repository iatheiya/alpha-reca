//aspects.re — The 24 Aspects of Reca (hardware ABI)

These are the only operations irreducible from others.
Each maps to one CPU instruction or a fixed OS escape.
IDs are stable within any compiled binary — changing them redefines Reca.

── Why exactly these 24 ────────────────────────────────────────────────────

The selection rule: an operation belongs here if and only if:
  1. It is natively available on the vast majority of target CPUs.
  2. Emulating it via other aspects has a real cost — either the CPU
     lacks the instruction entirely, or the native form is genuinely
     faster than any combination of the others.

Counterexample — Greater (>):
  Most CPUs do NOT have a dedicated "greater than" instruction.
  They have Less (<) and Equal (==). Greater is emulated as Less(b, a)
  at zero extra cost. So Greater is NOT in aspects.re.
  It lives in an Aria where it can be defined as Less with swapped els.

Counterexample — Not (bitwise complement):
  Many CPUs implement NOT as XOR with -1. On architectures that have a
  native NOT, using XOR costs the same number of cycles. We include it
  because the Reca model makes single-operand forms cleaner to express.

The 24 are the MAXIMUM universal set, not the minimum. Every aspect
here must be present or emulatable on every supported target. Adding
a 25th or removing one redefines Reca.

── Load-time primitives (not aspects) ──────────────────────────────────────

At load time (bootstrap, not runtime), three operations are the minimum:
  "NEW"  — allocate a Lux in Aether
  "SET"  — write a value into a Lux
  "LINK" — connect two Lux with a relation
All other loader commands (ITO, BLOCK, NEWREF, SETREF, NOLINK) are
combinations of these three. They exist in the loader for performance
during bootstrap, but conceptually they are not primitives.
After self-hosting, parser.re expresses them through NEW + SET + LINK.

── All aspects use NEWREF ───────────────────────────────────────────────────

All aspects use NEWREF so aether[aspect_addr] = aspect_addr (self-ref).
This ensures their address can be stored/read via Write/Read and compared
correctly with Equal. Without self-ref, aether[aspect_addr] = 0 and all
comparisons like "Equal El2=Move" would produce wrong results.

── Signed/unsigned naming ───────────────────────────────────────────────────

Div, Rem, Less  = signed   (sdiv, srem, icmp slt)
UDiv, URem, ULess  = unsigned  (udiv, urem, icmp ult)
Right  = logical right shift (lshr, zero-fills MSB)
ARight = arithmetic right shift (ashr, sign-extends MSB)

── Calling convention ───────────────────────────────────────────────────────

Voca saves the next-instruction address to RA_LINK (runtime/registers.re),
then jumps to El1.lux. Redi jumps to RA_LINK.lux.
The call stack (RA_SP) is automatic: Voca pushes the OLD RA_LINK onto
aether[RA_SP] before overwriting it; Redi pops it back after jumping.
This makes nested/recursive calls correct with zero per-function
bookkeeping. See runtime/registers.re for details.//

── Aether (IDs 1–2) ─────────────────────────────────────────────────────────

NEWREF Read

NEWREF Write

── Arithmetic (IDs 3–9) ─────────────────────────────────────────────────────

NEWREF Add

NEWREF Sub

NEWREF Mul

NEWREF Div

NEWREF Rem

NEWREF UDiv

NEWREF URem

── Bitwise (IDs 10–15) ──────────────────────────────────────────────────────

NEWREF And

NEWREF Or

NEWREF Xor

NEWREF Left

NEWREF Right

NEWREF ARight

── Comparison (IDs 16–18) ───────────────────────────────────────────────────

NEWREF Equal

NEWREF Less

NEWREF ULess

── Control flow (IDs 19–20) ─────────────────────────────────────────────────

NEWREF JumpIf

NEWREF JumpReg

── System (IDs 21–22) ───────────────────────────────────────────────────────

NEWREF End

NEWREF Exire

── Procedure (IDs 23–24) ────────────────────────────────────────────────────

NEWREF Voca

NEWREF Redi
