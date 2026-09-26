//
aspects.re — The 25 Aspects of Reca (hardware ABI)

These are the only *runtime* operations irreducible from others.
Each maps to one CPU instruction or a fixed OS escape.
IDs are stable within any compiled binary; changing them is a real,
deliberate decision, not a casual one — same weight as any other
foundational choice, no more, no less.

── Why exactly these (currently 25) ────────────────────────────────────────

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

Idem is not a counterexample of the same kind, but was nearly left out
for a wrong reason the first time it was proposed: it looked "specific
to Reca's own model" rather than to hardware. It is not — it is a
different *class* of native instruction than the rest of this file
happened to cover so far (memory load/store, arithmetic, bitwise,
comparison, control flow). Immediate load — placing an already-known
constant somewhere with zero memory reads — is exactly as native and
universal as memory load (x86 MOV reg,imm / ARM MOV,MOVZ / RISC-V LI,
all zero-extra-cost relative to a memory load), and every other aspect
here requires at least one dereference, so none of them can express it
without a real, avoidable extra read. A previously-missing category is
not the same thing as "not universal" — the 24 (now 25) were never
claimed to be complete on the first pass, only maximal *for the
categories considered so far*.

The set here is the MAXIMUM universal set for *runtime* execution
found so far, not a fixed, closed minimum. Every aspect here must be
present or emulatable on every supported target's own hardware.
Something specific to Reca's own model rather than to hardware in
general — needed by this language, not by CPUs generally — does not
belong here even if it would be cheap to run; it belongs in an Aria, or
(if it's about resolving something once, before the program runs at
all) among the load-time primitives below.

── Loading has no separate minimum — it is aspects + a one-time seed ────────

An earlier version of this file claimed loading had its own irreducible
minimum — NEW/SET/LINK — on the reasoning that "you cannot express
allocate a new Lux using only runtime aspects, because every aspect
already assumes the Lux it operates on exists; NEW can't be built from
NEW." That reasoning was wrong, and the separate category with it.

NEW/SET/LINK are derivable, like every other loader command:
  SET  — is the Write aspect, nothing more.
  NEW  — is ALLOC + register the name; and ALLOC is Read the bump cursor,
         Add N, Write it back — pure runtime aspects on one ordinary cell
         (K_CURSOR). "NEW can't be built from NEW" mistook the cursor for
         a Lux: it is a cell aspects read and write like any other, so
         allocation IS expressible without circularity.
  LINK — is ALLOC an edge cell + Write the (rel, tgt) pair into it
         (ADD_LUMEN, alloc.re) — again only allocation and writes.

What is actually irreducible is exactly the runtime floor, nothing added
for loading: the 25 aspects, plus the execution loop's own contract for
reading a cell as a NOVA (op/e1/e2/exit/next) and dispatching its aspect
— the machine itself, not a command. The one thing loading needs beyond
that is a bootstrap SEED: the substrate lays the first NOVA cells and the
initial cursor directly, by fiat, because an aspect needs a NOVA to hold
it while a NOVA is made by running aspects — a circularity broken once,
at the start, by the seed (freeze). That seed is content laid a single
time, not a standing "load-time aspect."

NEW/SET/LINK survive as named loader commands only for bootstrap
readability and speed, and every other command (NOVA, CLEAR, NOP, JEQ,
JZ, ALLOC_TO, and the rest) is built from them — but none of the three is
conceptually irreducible; each bottoms out in aspects like all the rest.
(SEAL and SPAN, once listed here too, were retired — both were pure
subsets of SET/NOVA and are gone as commands.)

── All aspects self-ref via SET ─────────────────────────────────────────────

All aspects are self-ref'd via SET (aether[aspect_addr] = aspect_addr).
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
bookkeeping. See runtime/registers.re for details.
//

── Aether (IDs 1–2) ─────────────────────────────────────────────────────────

SET Read

SET Write

── Arithmetic (IDs 3–9) ─────────────────────────────────────────────────────

SET Add

SET Sub

SET Mul

SET Div

SET Rem

SET UDiv

SET URem

── Bitwise (IDs 10–15) ──────────────────────────────────────────────────────

SET And

SET Or

SET Xor

SET Left

SET Right

SET ARight

── Comparison (IDs 16–18) ───────────────────────────────────────────────────

SET Equal

SET Less

SET ULess

── Control flow (IDs 19–20) ─────────────────────────────────────────────────

SET JumpIf

SET JumpReg

── System (IDs 21–22) ───────────────────────────────────────────────────────

SET End

SET Exire

── Procedure (IDs 23–24) ────────────────────────────────────────────────────

SET Voca

SET Redi

── Immediate (ID 25) ────────────────────────────────────────────────────────

//
Idem(a) = a, verbatim — zero dereferences. Every other aspect above
reads through at least one address (Move/Add/etc. read El1 once;
Write/Read read El1 twice). Idem reads through none: the operand's own,
already-resolved, compile-time-known value is written directly, the
same way an immediate-load CPU instruction places a known constant
somewhere without a memory read. Existed as a real, felt need well
before it had a name or a home — every "_REF" holder lux project-wide
(SEAL X_REF X, used purely so Move/Equal would have something to
correctly dereference down to X's own address) was working around its
absence. Idem does not replace that pattern's *result* (a caller who
wants X's address still gets it) — it replaces the *mechanism*: no
holder lux, no extra dereference, no per-call-site "do I need a _REF
here" bookkeeping. See BUGS.md Pattern Z and ROADMAP.md D6 for the full
reasoning trail.
//

SET Idem
