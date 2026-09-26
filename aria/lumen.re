//
lumen.re — Lumen iteration subroutines

Lux   = u64 at position 0 of any lux.
Lumen = (rel, exit) pair at positions 1..2n of a data lux.
aether[lux + 1] = rel_addr of first lumen  (or 0 = no lumina)
aether[lux + 2] = exit_addr of first lumen
aether[lux + 3] = rel_addr of second lumen (or 0 = end)
//
//
The 0 after the last pair is automatic (bump zeroes aether).

Instruction lux (NOVA) layout is different (fixed positional, no rel stored):
[word(self-ref), op, e1, e2, exit, next, pad]  — 7 luces (NOVA_SIZE), explicit SLOT_NEXT.
pad (slot 6) is 0 when there are no extra lumen, exactly as before; when
there ARE extra lumen, they start AT slot 6 itself (not after it) as
(rel,exit) pairs, 0-terminated -- only consulted when SLOT_NEXT==0.

── Subroutines ───────────────────────────────────────────────────────────────
SR_LUX_OF    IN: RA_SR_LUX              → RA_SR_OUT = lux value
SR_FIRST_LM  IN: RA_SR_LUX              → RA_SR_POS = lux+1 (rel pos)
RA_SR_REL = first rel (or 0)
RA_SR_OUT = first exit (or 0)
SR_NEXT_LM   IN: RA_SR_POS              → RA_SR_POS += 2
RA_SR_REL = next rel (or 0)
RA_SR_OUT = next exit (or 0)
SR_LM_REL    IN: RA_SR_POS              → RA_SR_REL = rel at pos
SR_LM_EXIT    IN: RA_SR_POS              → RA_SR_OUT = exit at pos+1
SR_GLL       → SR_FIRST_LM  (LH macro entry; sets RA_SR_OFFSET=C_1 first)
SR_GLR       IN: RA_SR_LUMEN            → RA_SR_REL, RA_SR_OUT = rel
SR_GLE       IN: RA_SR_LUMEN            → RA_SR_OUT = exit (pos+1)
SR_GLX       IN: RA_SR_LUMEN            → RA_SR_LUMEN += 2, RA_SR_REL = next rel
SR_WALK_ONE  IN: RA_SR_LUX, RA_SR_REL  → RA_SR_OUT = exit where rel matches, or 0
SR_COUNT_LM  IN: RA_SR_LUX              → RA_SR_OUT = number of lumina

DEPENDENCY: aspects.re  constants.re  registers.re
//

NEW RA_SR_LUX  /input: lux address
NEW RA_SR_POS  /input/output: current position (points to rel lux of a pair)
NEW RA_SR_OFFSET  /input: offset from lux start to first lumen (1 for Data, NOVA_SIZE for NOVA)
NEW RA_SR_REL  /input (SR_WALK_ONE) / output (SR_FIRST_LM, SR_NEXT_LM, SR_LM_REL, SR_GLR, SR_GLX)
NEW RA_SR_OUT  /output: exit addr or result value
NEW RA_SR_LUMEN  /current lumen pair position (rel lux addr) — used by LR/LT/LX macros

── SR_LUX_OF: read lux of lux RA_SR_LUX → RA_SR_OUT ───────────────────────
/Lux is at position 0 (aether[lux]). Leaf.
NOLINK
NOVA SR_LUX_OF Op=Read El1=RA_SR_LUX Exit=RA_SR_OUT
RREDI SR_LUX_OF2

── SR_FIRST_LM: first lumen pair of RA_SR_LUX ───────────────────────────────
//
Sets RA_SR_POS = lux+offset. Reads rel → RA_SR_REL. Reads exit → RA_SR_OUT.
If OVERFLOW_REL encountered: automatically follows chain (jumps to overflow lux pos 1).
If no lumina (or chain end): RA_SR_REL = 0, RA_SR_OUT = 0. Leaf for normal case.
//
NOLINK
NOVA SR_FIRST_LM Op=Add El1=RA_SR_LUX El2=RA_SR_OFFSET Exit=RA_SR_POS
NOVA SR_FL_REL Op=Read El1=RA_SR_POS Exit=RA_SR_REL
/Check OVERFLOW_REL
JEQ SR_FL_OVCK El1=RA_SR_REL El2=OVERFLOW_REL Exit=SR_FL_OVERFLOW
NOVA SR_FL_TPOS Op=Add El1=RA_SR_POS El2=C_1 Exit=RA_SR_LUMEN
NOVA SR_FL_EXIT Op=Read El1=RA_SR_LUMEN Exit=RA_SR_OUT
RREDI SR_FL_RET
/Follow overflow chain at first position
NOLINK
NOVA SR_FL_OVERFLOW Op=Add El1=RA_SR_POS El2=C_1 Exit=RA_SR_LUMEN
NOVA SR_FL_OVRD Op=Read El1=RA_SR_LUMEN Exit=RA_SR_REL /read overflow lux addr
JZ SR_FL_OVZ El1=RA_SR_REL Exit=SR_FL_NOMATCH /exit==0 → no overflow → end
/Jump into overflow lux from pos 1
NOVA SR_FL_OVCONT Op=Add El1=RA_SR_REL El2=C_1 Exit=RA_SR_POS
NOVA Op=Read El1=RA_SR_POS Exit=RA_SR_REL
NOVA Op=Add El1=RA_SR_POS El2=C_1 Exit=RA_SR_LUMEN
NOVA Op=Read El1=RA_SR_LUMEN Exit=RA_SR_OUT

RREDI SR_FL_OVRET
NOLINK
CLEAR SR_FL_NOMATCH El1=RA_SR_REL
CLEAR SR_FL_NM_OUT El1=RA_SR_OUT
RREDI SR_FL_NMRET

── SR_NEXT_LM: advance to next lumen pair ───────────────────────────────────
//
RA_SR_POS += 2. Reads rel → RA_SR_REL. Reads exit → RA_SR_OUT.
Follows OVERFLOW_REL chain: when rel==OVERFLOW_REL, jumps to overflow lux. Leaf.
//
NOLINK
NOVA SR_NEXT_LM Op=Add El1=RA_SR_POS El2=C_2 Exit=RA_SR_POS
NOVA SR_NL_REL Op=Read El1=RA_SR_POS Exit=RA_SR_REL
/Check OVERFLOW_REL
JEQ SR_NL_OVCK El1=RA_SR_REL El2=OVERFLOW_REL Exit=SR_NL_OVERFLOW
NOVA SR_NL_TPOS Op=Add El1=RA_SR_POS El2=C_1 Exit=RA_SR_LUMEN
NOVA SR_NL_EXIT Op=Read El1=RA_SR_LUMEN Exit=RA_SR_OUT
RREDI SR_NL_RET
/Follow overflow chain
NOLINK
NOVA SR_NL_OVERFLOW Op=Add El1=RA_SR_POS El2=C_1 Exit=RA_SR_LUMEN
NOVA SR_NL_OVRD Op=Read El1=RA_SR_LUMEN Exit=RA_SR_REL /overflow lux addr
JZ SR_NL_OVZ El1=RA_SR_REL Exit=SR_NL_END /exit==0 → end of chain
NOVA SR_NL_OVCONT Op=Add El1=RA_SR_REL El2=C_1 Exit=RA_SR_POS
NOVA Op=Read El1=RA_SR_POS Exit=RA_SR_REL
NOVA Op=Add El1=RA_SR_POS El2=C_1 Exit=RA_SR_LUMEN
NOVA Op=Read El1=RA_SR_LUMEN Exit=RA_SR_OUT

RREDI SR_NL_OVRET
NOLINK
CLEAR SR_NL_END El1=RA_SR_REL
CLEAR SR_NL_ENDOUT El1=RA_SR_OUT
RREDI SR_NL_ENDRET

── SR_LM_REL: read rel at RA_SR_POS → RA_SR_REL ────────────────────────────
/Leaf.
NOLINK
NOVA SR_LM_REL Op=Read El1=RA_SR_POS Exit=RA_SR_REL
RREDI SR_LM_REL2

── SR_LM_EXIT: read exit at RA_SR_POS+1 → RA_SR_OUT ─────────────────────────
/Leaf.
NOLINK
NOVA SR_LM_EXIT Op=Add El1=RA_SR_POS El2=C_1 Exit=RA_SR_LUMEN
NOVA SR_LT_READ Op=Read El1=RA_SR_LUMEN Exit=RA_SR_OUT
RREDI SR_LT_RET

── SR_GLL: entry point for Data lux lumen scan (offset=1) ─────────────────
//
Sets RA_SR_OFFSET=C_1 then falls into SR_FIRST_LM.
Used by LH macro and any caller scanning a Data lux lumen list.
Prefer SR_GLL over a manual Move+Jump to make the Data intent explicit.
//
NOLINK
NOVA SR_GLL Op=Move El1=C_1 Exit=RA_SR_OFFSET
NOVA SR_GLL_J Op=Jump Exit=SR_FIRST_LM

── SR_GLR: read rel at RA_SR_LUMEN → RA_SR_REL and RA_SR_OUT ───────────────
/IN: RA_SR_LUMEN = pos of current lumen pair (rel lux). Leaf.
NOLINK
NOVA SR_GLR Op=Read El1=RA_SR_LUMEN Exit=RA_SR_REL
NOVA SR_GLR_OUT Op=Move El1=RA_SR_REL Exit=RA_SR_OUT
RREDI SR_GLR_RET

── SR_GLE: read exit at RA_SR_LUMEN+1 → RA_SR_OUT ───────────────────────────
//
IN: RA_SR_LUMEN = pos of current lumen pair (rel lux). Leaf.
Uses RA_SR_OUT as intermediate address so RA_SR_POS is not clobbered.
//
NOLINK
NOVA SR_GLE Op=Add El1=RA_SR_LUMEN El2=C_1 Exit=RA_SR_OUT
NOVA SR_GLE_LUX Op=Read El1=RA_SR_OUT Exit=RA_SR_OUT
RREDI SR_GLE_RET

── SR_GLX: advance RA_SR_LUMEN by 2 to next pair → RA_SR_LUMEN, RA_SR_REL ──
//
IN: RA_SR_LUMEN = pos of current rel lux. Leaf for normal case.
Follows OVERFLOW_REL: jumps to overflow lux pos 1 when encountered.
//
NOLINK
NOVA SR_GLX Op=Add El1=RA_SR_LUMEN El2=C_2 Exit=RA_SR_LUMEN
NOVA SR_GLX_REL Op=Read El1=RA_SR_LUMEN Exit=RA_SR_REL
JEQ SR_GLX_OVCK El1=RA_SR_REL El2=OVERFLOW_REL Exit=SR_GLX_OVERFLOW
NOVA SR_GLX_OUT Op=Move El1=RA_SR_REL Exit=RA_SR_OUT
RREDI SR_GLX_RET
NOLINK
NOVA SR_GLX_OVERFLOW Op=Add El1=RA_SR_LUMEN El2=C_1 Exit=RA_SR_LUMEN
NOVA SR_GLX_OVRD Op=Read El1=RA_SR_LUMEN Exit=RA_SR_REL
JZ SR_GLX_OVZ El1=RA_SR_REL Exit=SR_GLX_END
NOVA SR_GLX_CONT Op=Add El1=RA_SR_REL El2=C_1 Exit=RA_SR_LUMEN
NOVA SR_GLX_OREL Op=Read El1=RA_SR_LUMEN Exit=RA_SR_REL
NOVA SR_GLX_OOUT Op=Move El1=RA_SR_REL Exit=RA_SR_OUT
RREDI SR_GLX_ORET
NOLINK
CLEAR SR_GLX_END El1=RA_SR_REL
CLEAR SR_GLX_EOUT El1=RA_SR_OUT
RREDI SR_GLX_ERET

── SR_WALK_ONE: find exit of first lumen with rel == RA_SR_REL ───────────────
//
IN: RA_SR_LUX = lux to search. RA_SR_REL = relation to match.
OUT: RA_SR_OUT = exit_addr if found, 0 if not found./
For Data luces: offset=1 (set via Move El1=C_1 Exit=RA_SR_OFFSET before call).
For NOVA extra-lumen: use WALK_NOVA macro (sets offset=C_6=SLOT_PAD automatically -- extras, when present, start right at the pad slot itself, not after it).
Non-leaf — RA_LINK is saved/restored automatically by the call stack;
RA_SR_REL is overwritten by SR_FIRST_LM/SR_NEXT_LM, saved here in RA_SR_WO_REL.
//
NEW RA_SR_WO_REL
NOLINK
NOVA SR_WALK_ONE Op=Move El1=C_1 Exit=RA_SR_OFFSET
NOVA SR_WO_SREL Op=Move El1=RA_SR_REL Exit=RA_SR_WO_REL
RVOCA SR_WO_FIRST Body=SR_FIRST_LM
JZ SR_WO_LOOP El1=RA_SR_REL Exit=SR_WO_MISS
JEQ SR_WO_CMP El1=RA_SR_REL El2=RA_SR_WO_REL Exit=SR_WO_FOUND
RVOCA SR_WO_NEXT Body=SR_NEXT_LM
NOVA SR_WO_LB Op=Jump Exit=SR_WO_LOOP

NOLINK
NOVA SR_WO_FOUND Op=Read El1=RA_SR_LUMEN Exit=RA_SR_OUT
RREDI SR_WO_FRET_r

NOLINK
CLEAR SR_WO_MISS El1=RA_SR_OUT
RREDI SR_WO_MRET_r

── SR_COUNT_LM: count lumina of lux RA_SR_LUX → RA_SR_OUT ─────────────────
/Walks pairs from pos 1, counts until rel == 0. Follows OVERFLOW_REL chain. Non-leaf.
NEW RA_SR_CNT
NOLINK
NOVA SR_COUNT_LM Op=Add El1=RA_SR_LUX El2=C_1 Exit=RA_SR_POS
CLEAR SC_ZERO El1=RA_SR_CNT

NOVA SC_LOOP Op=Read El1=RA_SR_POS Exit=RA_SR_REL
JZ SC_CKZ El1=RA_SR_REL Exit=SC_DONE
/Follow overflow chain
JEQ SC_OVCK El1=RA_SR_REL El2=OVERFLOW_REL Exit=SC_OVERFLOW
NOVA SC_INC Op=Add El1=RA_SR_CNT El2=C_1 Exit=RA_SR_CNT
NOVA SC_ADV Op=Add El1=RA_SR_POS El2=C_2 Exit=RA_SR_POS
NOVA SC_LB Op=Jump Exit=SC_LOOP
/Overflow: jump to overflow lux
NOLINK
NOVA SC_OVERFLOW Op=Add El1=RA_SR_POS El2=C_1 Exit=RA_SR_POS
NOVA SC_OVRD Op=Read El1=RA_SR_POS Exit=RA_SR_REL
JZ SC_OVZ El1=RA_SR_REL Exit=SC_DONE /no overflow lux → done
NOVA SC_OVCONT Op=Add El1=RA_SR_REL El2=C_1 Exit=RA_SR_POS
NOVA SC_OVJMP Op=Jump Exit=SC_LOOP

NOLINK
NOVA SC_DONE Op=Move El1=RA_SR_CNT Exit=RA_SR_OUT
RREDI SC_RET_r
