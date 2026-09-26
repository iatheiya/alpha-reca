//
constants.re — Common integer constants and lux layout offsets

── Lux model ────────────────────────────────────────────────────────────────

Lux   = u64   — one unit of information (position 0 of any lux)
Lumen = addr  — one connection (positions 1..n of a data lux)

Data lux:       [word, rel0, exit0, rel1, exit1, ..., 0]
word at pos 0, lumen pairs from pos 1 onward, 0-terminated.

"NOVA" lux (compact fixed-slot layout, 7 physical luces):
pos 0 = word  = self-ref (aether[addr] = addr)
pos 1 = op    = primitive addr (SLOT_OP)
pos 2 = e1    = first operand   (SLOT_E1)
pos 3 = e2    = second operand  (SLOT_E2)
pos 4 = exit  = exit/dest addr  (SLOT_EXIT)
pos 5 = next  = 0 → implicit fall-through (pc+NOVA_SIZE); !0 → explicit graph link (SLOT_NEXT)
pos 6 = pad   = 0 (terminates lumen scan before extra lumens)
pos 7+= extra LINK lumens: (rel, exit) pairs, 0-terminated

DEPENDENCY: aspects.re
//

── Pure numeric constants ────────────────────────────────────────────────────

SET C_0 Value=0

SET C_1 Value=1

SET C_2 Value=2

SET C_3 Value=3

SET C_4 Value=4

SET C_5 Value=5

SET C_6 Value=6

SET C_7 Value=7

SET C_8 Value=8

SET C_9 Value=9

SET C_10 Value=10
SET C_11 Value=11
SET C_12 Value=12
SET C_13 Value=13
SET C_14 Value=14
SET C_15 Value=15
SET C_16 Value=16
SET C_17 Value=17
SET C_18 Value=18
SET C_19 Value=19

/C_7_REF formerly declared here -- removed once NOVAE gained its own
/Op1=/Op2=/Op3=Idem operand-resolution mode (El1=C_7 Op1=Idem now used
/directly at the one real call site). See BUGS.md Pattern Z,
/REF_TO_IDEM.md.

SET C_32 Value=32

SET C_33 Value=33


SET C_48 Value=48

SET C_63 Value=63

SET C_64 Value=64

SET C_100 Value=100


SET C_255 Value=255

SET C_256 Value=256

SET C_512 Value=512

SET C_1024 Value=1024

//
removed unused: C_2048
//

SET C_5381 Value=5381

SET C_NEG1 Value=-1

── NOVA lux slot offsets ─────────────────────────────────────────────────────
//
Offsets from the NOVA lux base address to each fixed slot.
These match aether.py SLOT_* constants (single source of truth: aether.py).
//

SET SLOT_OP Value=1 /op primitive addr (dispatch key)

SET SLOT_E1 Value=2 /first operand addr

SET SLOT_E2 Value=3 /second operand addr

SET SLOT_EXIT Value=4 /exit addr (store target, jump target, RA_LINK addr for Voca)

SET SLOT_NEXT Value=5 /next NOVA: 0 = implicit fall-through (pc+NOVA_SIZE), !0 = explicit graph link

//
SLOT_PAD doubles as the extras signal: 0 (as always) = no extra
lumens, ordinary +NOVA_SIZE fall-through, unchanged and free. Non-zero
= extra LINK lumens start right AT this slot (not after it) -- the
same self-describing (rel, target) pairs a Data lux already uses,
0-terminated. Only checked when SLOT_NEXT==0 (the ordinary/sequential
case); an explicit SLOT_NEXT (warp/flux) is unrelated and unaffected.
See interpreter.py's _nova_extra_skip for the reference scan.
//
SET SLOT_PAD Value=6 /0 = no extras (as before); !=0 = extras start here

//
NOVA base Op=size: word+op+e1+e2+exit+next+pad = 7 luces.
//
SET NOVA_SIZE Value=7

//
── Flux slot type codes ────────────────────────────────────────────────
//
//
Flux = [descriptor, val1, val2, ..., 0] — one lux, no separate type lux.
descriptor is a sequence of 4-bit digits (nibbles), unpacked from the
least-significant nibble upward, 0-terminated. A plain digit (1-7 below)
consumes exactly one value, in order. FLUX_GROUP/FLUX_PIVOT (14/13)
mark a group -- "FLUX_GROUP code(s)... FLUX_PIVOT count FLUX_GROUP" --
always written in pairs (never omitted, even at digit 0: omitting the
opener makes every later FLUX_GROUP ambiguous between opening a new
group and closing the implicit one that silently started at 0, since
occurrence parity is the only thing that tells them apart). One code
before the pivot + count N after = "one-to-many" (that code gets N
separate values). Several codes before the pivot + count N after =
"many-to-one" (those codes share the same N value(s), N=1 in the
common case). See interpreter.py's _exec_flux for the reference decode.
//
//
Next (code 6) may repeat (plain, or via a one-to-many group) →
N continuation points (fan-out).
//
SET FLUX_SLOT_OP Value=2 /op (dispatch primitive)
SET FLUX_SLOT_E1 Value=3 /first element operand
SET FLUX_SLOT_E2 Value=4 /second element operand
SET FLUX_SLOT_EXIT Value=5 /exit / destination
SET FLUX_SLOT_NEXT Value=6 /next continuation point (may repeat)
SET FLUX_SLOT_REL Value=7 /relation (lumen rel)
SET FLUX_PIVOT Value=13 /inside a group: separates code(s) from count
SET FLUX_GROUP Value=14 /group boundary, always paired (open/close)
