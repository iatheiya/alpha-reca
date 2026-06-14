//constants.re — Common integer constants and lux layout offsets

── Lux model ────────────────────────────────────────────────────────────────

Lux   = u64   — one unit of information (position 0 of any lux)
Lumen = addr  — one connection (positions 1..n of a data lux)

Data lux:       [word, rel0, exit0, rel1, exit1, ..., 0]
word at pos 0, lumen pairs from pos 1 onward, 0-terminated.

ITO lux (compact fixed-slot layout, 7 physical luces):
pos 0 = word  = self-ref (aether[addr] = addr)
pos 1 = op    = primitive addr (SLOT_OP)
pos 2 = e1    = first operand   (SLOT_E1)
pos 3 = e2    = second operand  (SLOT_E2)
pos 4 = exit  = exit/dest addr  (SLOT_EXIT)
pos 5 = next  = 0 → implicit fall-through (pc+ITO_SIZE); !0 → explicit graph link (SLOT_NEXT)
pos 6 = pad   = 0 (terminates lumen scan before extra lumens)
pos 7+= extra LINK lumens: (rel, exit) pairs, 0-terminated

DEPENDENCY: aspects.re//

── Pure numeric constants ────────────────────────────────────────────────────

NEWSET C_0 0

NEWSET C_1 1

NEWSET C_2 2

NEWSET C_3 3

NEWSET C_4 4

NEWSET C_5 5

NEWSET C_6 6

NEWSET C_7 7

NEWSET C_8 8

NEWSET C_9 9

NEWSET C_10 10
NEWSET C_13 13
NEWSET C_11 11
NEWSET C_12 12
NEWSET C_14 14
NEWSET C_15 15
NEWSET C_16 16
NEWSET C_17 17
NEWSET C_18 18
NEWSET C_19 19

//removed unused
was SET C_18 18

removed unused
was SET C_20 20//


NEWSET C_32 32

NEWSET C_33 33


NEWSET C_48 48

NEWSET C_63 63

NEWSET C_64 64

NEWSET C_100 100


NEWSET C_255 255

NEWSET C_256 256

NEWSET C_512 512

NEWSET C_1024 1024

//removed unused
was SET C_2048 2048//

NEWSET C_5381 5381

NEWSET C_NEG1 -1

── ITO lux slot offsets ─────────────────────────────────────────────────────
//Offsets from the ITO lux base address to each fixed slot.
These match symphony.py SLOT_* constants (single source of truth: symphony.py).//

NEWSET SLOT_OP  1      /op primitive addr (dispatch key)

NEWSET SLOT_E1  2      /first operand addr

NEWSET SLOT_E2  3      /second operand addr

NEWSET SLOT_EXIT 4     /exit addr (store target, jump target, RA_LINK addr for Voca)

NEWSET SLOT_NEXT 5     /next ITO: 0 = implicit fall-through (pc+ITO_SIZE), !0 = explicit graph link

//ITO base size: word+op+e1+e2+exit+next+pad = 7 luces.
Extra LINK lumens (from LINK commands) start at slot 7 (ITO_SIZE).//
NEWSET ITO_SIZE 7

//── Flux slot type codes ────────────────────────────────────────────────
//Used in type lux (Data Lux) to describe flux structure.
//Type lux = [word, code1, code2, ..., 0] — sequence of codes, 0-terminated.
//Codes describe what occupies each slot of the flux, in order.
//Next (code 6) may repeat N times → N continuation points (fan-out).//
NEWSET FLUX_SLOT_OP    2   /op (dispatch primitive)
NEWSET FLUX_SLOT_E1    3   /first element operand
NEWSET FLUX_SLOT_E2    4   /second element operand
NEWSET FLUX_SLOT_EXIT  5   /exit / destination
NEWSET FLUX_SLOT_NEXT  6   /next continuation point (may repeat)
NEWSET FLUX_SLOT_REL   7   /relation (lumen rel)