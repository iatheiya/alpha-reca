/macros.re — Load-time macros as Reca-programs (parallel to Python inline)
/
/Each CMD_MACRO is a Reca-program. Loader calls it when CMD is seen in Wave-B.
/Loader writes resolved els into RA_MA0..RA_MA3 then executes CMD_MACRO.
/
/Load-time aspects (freeze-only):
/  __LT_GET_LAST — RA_MA_RET = _last_ito
/  __LT_SET_LAST — _last_ito = aether[RA_MA0]
/  __LT_REG_SYM  — register symbol name(MA0)=lux(MA1) in Python symbols
/
/ITO slot offsets: C_1=op C_2=e1 C_3=e2 C_4=exit C_5=next
/Write El1=REG El2=VAL → aether[aether[REG]] = aether[VAL]

NEW RA_MC_LUX      /scratch: current lux being built
NEW RA_MC_PREV      /scratch: previous lux for auto-link (synced with loader._last_ito)
NEW RA_MC_SLOT      /scratch: slot address (lux_addr + offset)
NEW __LT_ALLOC_ITO  /load-time only: bump-allocate one anonymous ITO lux, return addr in RA_MA_RET
                    /Python injects the real handler into interpreter dispatch during _call_reca_macro.
                    /At runtime this lux is dead (handler removed after each macro call).

/── WIRE_AUTOLINK: wire RA_MC_LUX (op/e1/e2/exit) then autolink prev→lux ────
/Caller sets: RA_MC_LUX, RA_MC_OP, RA_MC_E1, RA_MC_E2, RA_MC_DEST, then RVOCA.
/Writes all 5 slots, then calls AUTOLINK (updates RA_MC_PREV).
NEWREF WIRE_AUTOLINK WIS_START       /alias: same body as WRITE_ITO_SLOTS
NEWREF WIRE_AUTOLINK_RESET AUTORST_JCK /alias: entry of AUTOLINK_RESET
/── RVOCA: RVOCA name sub → ITO name Voca El1=sub Exit=RA_LINK
/MA0=name_addr  MA1=sub_addr
NEWREF RVOCA RVOCA_SPN
WAVE RVOCA_SPN  Voca  El1=RA_MA1  Exit=RA_LINK_REF  At=RA_MA0
RREDI RVOCA_RET
/── RREDI: RREDI name → ITO name Redi El1=RA_LINK + reset _last_ito
/MA0=name_addr
NEWREF RREDI RREDI_SPN
WAVE RREDI_SPN  Redi  El1=RA_LINK_REF  At=RA_MA0  Reset=1
RREDI RREDI_RET
/── CLEAR: CLEAR name target → ITO name Move El1=C_0 Exit=target
/MA0=name_addr  MA1=target_addr
NEWREF CLEAR CLEAR_SPN
WAVE CLEAR_SPN  Move  El1=RA_C0_REF  Exit=RA_MA1  At=RA_MA0
RREDI CLEAR_RET
/── NOP: NOP name → ITO name Move El1=C_0 Exit=C_0
/MA0=name_addr
NEWREF NOP NOP_SPN
WAVE NOP_SPN  Move  El1=RA_C0_REF  Exit=RA_C0_REF  At=RA_MA0
RREDI NOP_RET
/── ALLOC_TO: ALLOC_TO name dest count → Move count→RA_ALLOC_COUNT; ALLOC_LUCES; Move result→dest
/MA0=name MA1=dest MA2=count MA3=name_J(auto _J) MA4=name_K(auto _K)
NEWREF ALLOC_TO ALLOC_TO_N1
/── ALLOC_TO name dest size [_J] [_K] ───────────────────────────────────────
/Synthesizes 3 ITOs: name(Move size→ALLOC_COUNT), _J(Voca ALLOC_LUCES), _K(Move RESULT→dest).
NOLINK
WAVE ALLOC_TO_N1  Move  El1=RA_MA2  Exit=RA_ALLOC_COUNT_REF  At=RA_MA0  Next=RA_MA3
WAVE ALLOC_TO_N2  Voca  El1=ALLOC_LUCES  Exit=RA_LINK_REF       At=RA_MA3  Next=RA_MA4
WAVE ALLOC_TO_N3  Move  El1=RA_ALLOC_RESULT_REF  Exit=RA_MA1    At=RA_MA4
RREDI ALLOC_TO_RET

NEW RA_MC_FLAG   /scratch: flag data lux addr (for JEQ/JZ)
SETREF RA_MC_FLAG RA_MC_FLAG  /self-ref
NEW RA_MC_OP     /scratch: op addr
NEW RA_MC_E1      /scratch: arg A
NEW RA_MC_E2      /scratch: arg B
NEW RA_MC_DEST   /scratch: destination addr
NEW RA_MC_J      /scratch: second lux (_J suffix)
NEW RA_MC_K      /scratch: third lux (_K suffix)
/── WRITE_ITO_SLOTS: write all 5 ITO slots then wire autolink ──
/IN: RA_MC_LUX=addr, RA_MC_OP=op, RA_MC_E1=e1, RA_MC_E2=e2, RA_MC_DEST=exit
/Writes: word(self), op, e1, e2, exit. Then calls WIRE_AUTOLINK.
/Relies solely on the automatic RA_SP call stack to preserve RA_LINK across
/the nested WIRE_AUTOLINK call — safe to call from JEQ/JZ/LINK etc. at any
/nesting depth, with no scratch-register clobbering risk.
NEWREF WRITE_ITO_SLOTS WIS_START
CHAIN WIS_START
    Move  El1=RA_MC_LUX  Exit=RA_MC_SLOT
    Write El1=RA_MC_SLOT El2=RA_MC_LUX
    Add   El1=RA_MC_LUX  El2=C_1        Exit=RA_MC_SLOT
    Write El1=RA_MC_SLOT El2=RA_MC_OP
    Add   El1=RA_MC_LUX  El2=C_2        Exit=RA_MC_SLOT
    Write El1=RA_MC_SLOT El2=RA_MC_E1
    Add   El1=RA_MC_LUX  El2=C_3        Exit=RA_MC_SLOT
    Write El1=RA_MC_SLOT El2=RA_MC_E2
    Add   El1=RA_MC_LUX  El2=C_4        Exit=RA_MC_SLOT
    Write El1=RA_MC_SLOT El2=RA_MC_DEST
    Voca  El1=AUTOLINK Exit=RA_LINK
NOLINK
RREDI WIS_RET

/── WRITE_ITO_SLOTS_RESET: like WRITE_ITO_SLOTS but calls WIRE_AUTOLINK_RESET ──
/Used by chain terminators (RCALL_AT name_J, RREDI) — resets RA_MC_PREV after linking.
NEWREF WRITE_ITO_SLOTS_RESET WISR_START
CHAIN WISR_START
    Move  El1=RA_MC_LUX  Exit=RA_MC_SLOT
    Write El1=RA_MC_SLOT El2=RA_MC_LUX
    Add   El1=RA_MC_LUX  El2=C_1        Exit=RA_MC_SLOT
    Write El1=RA_MC_SLOT El2=RA_MC_OP
    Add   El1=RA_MC_LUX  El2=C_2        Exit=RA_MC_SLOT
    Write El1=RA_MC_SLOT El2=RA_MC_E1
    Add   El1=RA_MC_LUX  El2=C_3        Exit=RA_MC_SLOT
    Write El1=RA_MC_SLOT El2=RA_MC_E2
    Add   El1=RA_MC_LUX  El2=C_4        Exit=RA_MC_SLOT
    Write El1=RA_MC_SLOT El2=RA_MC_DEST
    Voca  El1=AUTOLINK_RESET Exit=RA_LINK
NOLINK
ITO WISR_RET  Redi El1=RA_LINK
/NOTE: raw ITO Redi — implementation of reset mechanism, not user of it.

NEWREF AUTOLINK AUTOLINK_JCK
/── AUTOLINK: sub — if RA_MC_PREV!=0 AND not physically adjacent to RA_MC_LUX,
/link prev→lux (SLOT_NEXT). If adjacent (RA_MC_LUX == RA_MC_PREV+ITO_SIZE),
/leave SLOT_NEXT=0 (already 0 from bump-allocation) so the interpreter's
/implicit fall-through (pc+ITO_SIZE) applies — zero runtime cost, the common case.
/Always sets _last_ito=RA_MC_LUX afterward, regardless of adjacency.
NOLINK
ITO AUTOLINK_JCK  JumpIf El1=RA_MC_PREV   Exit=AUTOLINK_ADJCK
ITO AUTOLINK_SKP  Jump   Exit=AUTOLINK_SET
NOLINK
ITO AUTOLINK_ADJCK Add   El1=RA_MC_PREV   El2=ITO_SIZE    Exit=RA_MC_SLOT
ITO AUTOLINK_ADJEQ Equal El1=RA_MC_SLOT   El2=RA_MC_LUX   Exit=RA_MC_FLAG
ITO AUTOLINK_ADJJ JumpIf El1=RA_MC_FLAG   Exit=AUTOLINK_SET
ITO AUTOLINK_DO   Add    El1=RA_MC_PREV   El2=SLOT_NEXT         Exit=RA_MC_SLOT
ITO AUTOLINK_DW   Write  El1=RA_MC_SLOT   El2=RA_MC_LUX
ITO AUTOLINK_SET  Move   El1=RA_MC_LUX   Exit=RA_MC_PREV
/NOTE: raw ITO Redi — this is the implementation of the autolink mechanism.
/All users of this mechanism use RREDI. The mechanism itself cannot.
ITO AUTOLINK_RET Redi El1=RA_LINK
NEWREF AUTOLINK_RESET AUTORST_JCK
/── AUTOLINK_RESET: like AUTOLINK but resets RA_MC_PREV to 0 after linking.
/Same adjacency check as AUTOLINK: skip the write when RA_MC_LUX is physically
/adjacent to RA_MC_PREV (fall-through line-path stays intact).
/Used by RREDI (chain terminator): links prev→lux, then clears prev so the
/next ITO is not auto-linked to the terminator.
NOLINK
ITO AUTORST_JCK  JumpIf El1=RA_MC_PREV   Exit=AUTORST_ADJCK
ITO AUTORST_SKP  Jump   Exit=AUTORST_RST
NOLINK
ITO AUTORST_ADJCK Add   El1=RA_MC_PREV   El2=ITO_SIZE    Exit=RA_MC_SLOT
ITO AUTORST_ADJEQ Equal El1=RA_MC_SLOT   El2=RA_MC_LUX   Exit=RA_MC_FLAG
ITO AUTORST_ADJJ JumpIf El1=RA_MC_FLAG   Exit=AUTORST_RST
ITO AUTORST_DO   Add    El1=RA_MC_PREV   El2=SLOT_NEXT         Exit=RA_MC_SLOT
ITO AUTORST_DW   Write  El1=RA_MC_SLOT   El2=RA_MC_LUX
ITO AUTORST_RST  Move   El1=C_0          Exit=RA_MC_PREV
/NOTE: raw ITO Redi — same reason as AUTOLINK_RET above. Implementation, not user.
ITO AUTORST_RET Redi El1=RA_LINK
/── JEQ: JEQ name a b dest → Equal(a,b)→RA_JEQ_FLAG; JumpIf→dest
/  MA0=name  MA1=a  MA2=b  MA3=dest  MA4=name_J(auto _J)
/Optimized: no _K NOP. JumpIf's own slot 5 (next, used by interpreter when
/condition is false) IS the fall-through point — autolink leaves RA_MC_PREV
/at name_J, so the next ordinary ITO in the file links into name_J.next,
/which is exactly where JumpIf jumps when a!=b. No extra lux needed.
NOLINK
NEWREF JEQ JEQ_N1
/── JEQ name a b dest [_J] ─────────────────────────────────────────────────
/Synthesizes 2 ITOs: name(Equal a b→FLAG), _J(JumpIf FLAG→dest).
/Explicit next-link: name→_J via slot 5 of name lux.
NOLINK
WAVE JEQ_N1  Equal   El1=RA_MA1  El2=RA_MA2  Exit=RA_JEQ_FLAG_PTR  At=RA_MA0  Next=RA_MA4
WAVE JEQ_N2  JumpIf  El1=RA_JEQ_FLAG_PTR  Exit=RA_MA3  At=RA_MA4
RREDI JEQ_RET
NOLINK

NEWREF JZ JZ_N1
/── JZ name x dest [_J] ─────────────────────────────────────────────────────
/Synthesizes 2 ITOs: name(Equal x C_0→FLAG), _J(JumpIf FLAG→dest).
NOLINK
WAVE JZ_N1   Equal   El1=RA_MA1  El2=RA_C0_REF  Exit=RA_JEQ_FLAG_PTR  At=RA_MA0  Next=RA_MA4
WAVE JZ_N2   JumpIf  El1=RA_JEQ_FLAG_PTR  Exit=RA_MA3  At=RA_MA4
RREDI JZ_RET
NOLINK

NEWREF LX LX_N1
/── LX lux src dest ──────────────────────────────────────────────────────────
/Synthesizes 3 ITOs: lux(Move src→SR_LUMEN), MA3(Voca SR_GLX), MA4(Move SR_LUMEN→dest)
NOLINK
WAVE LX_N1  Move  El1=RA_MA1  Exit=RA_SR_LUMEN  At=RA_MA0
WAVE LX_N2  Voca  El1=SR_GLX  Exit=RA_LINK_REF  At=RA_MA3
WAVE LX_N3  Move  El1=RA_SR_LUMEN  Exit=RA_MA2  At=RA_MA4
RREDI LX_RET
NOLINK

NEWREF LH LH_N0
/── LH lux src dest ──────────────────────────────────────────────────────────
/Synthesizes 4 ITOs: MA5(Move 1→SR_OFFSET), lux(Move src→SR_LUX), MA3(Voca SR_GLL), MA4(Move SR_LUMEN→dest)
NOLINK
WAVE LH_N0  Move  El1=C_1  Exit=RA_SR_OFFSET  At=RA_MA5
WAVE LH_N1  Move  El1=RA_MA1  Exit=RA_SR_LUX  At=RA_MA0
WAVE LH_N2  Voca  El1=SR_GLL  Exit=RA_LINK_REF  At=RA_MA3
WAVE LH_N3  Move  El1=RA_SR_LUMEN  Exit=RA_MA2  At=RA_MA4
RREDI LH_RET
NOLINK

NEWREF WALK_ONE WALK_N1
/── WALK_ONE lux rel dest MA3 MA4 MA5 ───────────────────────────────────────
/Synthesizes 4 ITOs for walking one lumen step.
NOLINK
WAVE WALK_N1  Move  El1=RA_MA1  Exit=RA_SR_LUX  At=RA_MA3
WAVE WALK_N2  Move  El1=RA_MA2  Exit=RA_SR_REL  At=RA_MA4
WAVE WALK_N3  Move  El1=C_1  Exit=RA_SR_OFFSET  At=RA_MA5
WAVE WALK_N4  Voca  El1=SR_WALK_ONE  Exit=RA_LINK_REF  At=RA_MA0
RREDI WALK_RET
NOLINK

NEWREF LINK_OP LINK_N1
/── LINK_OP lux src rel tgt MA4 MA5 MA6 ────────────────────────────────────
/Synthesizes 4 ITOs: lux(Move src→LM_SRC), MA4(Move rel→LM_REL), MA5(Move tgt→LM_EXIT), MA6(Voca ADD_LUMEN)
NOLINK
WAVE LINK_N1  Move  El1=RA_MA1  Exit=RA_LM_SRC  At=RA_MA0
WAVE LINK_N2  Move  El1=RA_MA2  Exit=RA_LM_REL  At=RA_MA4
WAVE LINK_N3  Move  El1=RA_MA3  Exit=RA_LM_EXIT  At=RA_MA5
WAVE LINK_N4  Voca  El1=ADD_LUMEN  Exit=RA_LINK_REF  At=RA_MA6
RREDI LINK_RET
NOLINK

NEWREF LR LR_N1
/── LR lux src dest ─────────────────────────────────────────────────────────
/Synthesizes 3 ITOs: lux(Move src→SR_LUMEN), _J(Voca SR_GLR), _K(Move SR_REL→dest).
NOLINK
WAVE LR_N1   Move  El1=RA_MA1  Exit=RA_SR_LUMEN_REF   At=RA_MA0  Next=RA_MC_J
WAVE LR_N2   Voca  El1=SR_GLR  Exit=RA_LINK_REF        At=RA_MC_J  Next=RA_MC_K
WAVE LR_N3   Move  El1=RA_SR_REL_REF  Exit=RA_MA2     At=RA_MC_K
ITO LR_SL    Move  El1=RA_MC_K  Exit=RA_MC_PREV
RREDI LR_RET
NOLINK

NEWREF LT LT_N1
/── LT lux src dest ─────────────────────────────────────────────────────────
/Synthesizes 3 ITOs: lux(Move src→SR_LUMEN), _J(Voca SR_GLE), _K(Move SR_OUT→dest).
NOLINK
WAVE LT_N1   Move  El1=RA_MA1  Exit=RA_SR_LUMEN_REF   At=RA_MA0  Next=RA_MC_J
WAVE LT_N2   Voca  El1=SR_GLE  Exit=RA_LINK_REF        At=RA_MC_J  Next=RA_MC_K
WAVE LT_N3   Move  El1=RA_SR_OUT_REF  Exit=RA_MA2     At=RA_MC_K
ITO LT_SL    Move  El1=RA_MC_K  Exit=RA_MC_PREV
RREDI LT_RET
NOLINK

NEWREF WALK_ITO WALK_ITO_N1
/── WALK_ITO lux src rel dest MA3 MA4 MA5 MA6 ───────────────────────────────
/Synthesizes 4 ITOs chained: lux(Move src→SR_LUX), _J(Move rel→SR_REL),
/_K(Move 7→SR_OFFSET), _L(Voca SR_WALK_ONE).
NOLINK
WAVE WALK_ITO_N1  Move  El1=RA_MA1      Exit=RA_SR_LUX_REF    At=RA_MA0    Next=RA_MC_J
WAVE WALK_ITO_N2  Move  El1=RA_MA2      Exit=RA_SR_REL_REF    At=RA_MC_J   Next=RA_MC_K
WAVE WALK_ITO_N3  Move  El1=C_7_REF    Exit=RA_SR_OFFSET_REF  At=RA_MC_K   Next=RA_MC_LUX
WAVE WALK_ITO_N4  Voca  El1=SR_WALK_ONE  Exit=RA_LINK_REF     At=RA_MC_LUX
RREDI WALK_ITO_RET
NOLINK

NEWREF UNLINK_OP UO_N1
/── UNLINK_OP lux src rel tgt MA4 MA5 MA6 ──────────────────────────────────
/Synthesizes 4 ITOs: lux(Move src→LM_SRC), MA4(Move rel→LM_REL), MA5(Move tgt→LM_EXIT), MA6(Voca REMOVE_LUMEN)
NOLINK
WAVE UO_N1  Move  El1=RA_MA1  Exit=RA_LM_SRC  At=RA_MA0
WAVE UO_N2  Move  El1=RA_MA2  Exit=RA_LM_REL  At=RA_MA4
WAVE UO_N3  Move  El1=RA_MA3  Exit=RA_LM_EXIT  At=RA_MA5
WAVE UO_N4  Voca  El1=REMOVE_LUMEN  Exit=RA_LINK_REF  At=RA_MA6
RREDI UNLINK_OP_RET
NOLINK

NEWREF RCALL_AT RCALL_AT_SPN
/── RCALL_AT name sub landing ──────────────────────────────────────────────
/Synthesizes a 7-ITO continuation block at runtime:
/  name:    Move RA_SP → RA_RCA_TMP          (save stack pointer)
/  name_P2: Sub  RA_RCA_TMP, FRAME_SIZE      (allocate frame)
/  name_P3: Move RA_RCA_TMP → RA_SP          (update stack pointer)
/  name_P4: Move RA_LINK → RA_RCA_TMP2       (save return address)
/  name_P5: Write RA_RCA_TMP, RA_RCA_TMP2    (push return addr onto stack)
/  name_P6: Move landing → RA_LINK           (redirect return to landing)
/  name_J:  Jump Exit=sub                    (call the subroutine)
/At=RA_MA0 on first WAVE: reuse the caller-provided name lux as entry point
/instead of allocating a new one. All subsequent WAVEs allocate fresh luces.
NOLINK
WAVE RCALL_AT_SPN Move  El1=RA_SP_REF        Exit=RA_RCA_TMP_REF   At=RA_MA0
WAVE RCALL_AT_P2  Sub   El1=RA_RCA_TMP_REF   El2=RA_FRAME_SIZE_REF Exit=RA_RCA_TMP_REF
WAVE RCALL_AT_P3  Move  El1=RA_RCA_TMP_REF   Exit=RA_SP_REF
WAVE RCALL_AT_P4  Move  El1=RA_LINK_REF      Exit=RA_RCA_TMP2_REF
WAVE RCALL_AT_P5  Write El1=RA_RCA_TMP_REF   El2=RA_RCA_TMP2_REF
WAVE RCALL_AT_P6  Move  El1=RA_MA2           Exit=RA_LINK_REF
/name_J: Jump to sub (chain terminator — WRITE_ITO_SLOTS_RESET via WAVE Reset=1)
/Next=RA_MC_J wires name_P6 → name_J. At=RA_MC_J + Reset=1 allocates and
/terminates the chain (WIRE_AUTOLINK_RESET resets RA_MC_PREV after linking).
CHAIN RCALL_AT_NL
    Add   El1=RA_MC_LUX  El2=SLOT_NEXT  Exit=RA_MC_SLOT
    Write El1=RA_MC_SLOT  El2=RA_MC_J
WAVE RCALL_AT_J  Jump  Exit=RA_MA1  At=RA_MC_J  Reset=1
RREDI RCALL_AT_RET
/── RCALL/RRET: REMOVED ──────────────────────────────────────────────────
/These macros implemented an inline call stack using a fixed 1024-entry
/array (BS_CS_BUF_000/BS_CS_SP in the now-removed bootstrap.re, since
/replaced by lexer.re/intern.re). Zero callers remain in the project — the
/automatic call stack (Voca/Redi push/pop RA_LINK on RA_SP, see
/runtime/registers.re) replaced this entirely. BS_CS_BUF_000/BS_CS_SP were
/removed outright, not moved to the replacement files.
/── RCN_IMPL: shared body for EMIT/EMITI/PUTBYTE ────────────────────────────
/IN: RA_MC_LUX=name_addr, RA_MA1=arg_addr,
/    RA_MC_OP=fn (EMIT_STR_ENTRY|EMIT_INT_ENTRY|PUT_BYTE),
/    RA_MC_DEST=reg_ref (RA_TW_LUX_REF|RA_TMP2_REF|RA_BYTE_REF).
/Wires: name(Move arg→reg_ref) → anon_J(Voca fn Exit=RA_LINK). Sets RA_MC_PREV=anon_J.
/Optimized: no anon_K NOP. Voca saves nxt (anon_J's slot 5) into RA_LINK on
/call; when fn returns via Redi(RA_LINK), execution resumes exactly at
/anon_J's slot 5 — the same place autolink would link the next ordinary
/ITO into. So leaving RA_MC_PREV=anon_J after this macro makes the next
/file line continue correctly without an extra NOP lux.
NEWREF RCN_IMPL RCN_S0
/── RCN_IMPL: shared body for EMIT/EMITI/PUTBYTE ───────────────────────────
/Synthesizes 2 ITOs:
/  name(Move RA_MA1→RA_MC_DEST) — written via direct slot-writes + AUTOLINK
/  _J  (Voca RA_MC_OP)          — synthesized via WAVE At=RA_MC_J, Next=linked
NOLINK
/First ITO: write slots directly (Move arg→reg_ref), then AUTOLINK
ITO RCN_S0     Move  El1=RA_MC_LUX   Exit=RA_MC_SLOT
ITO RCN_W0     Write El1=RA_MC_SLOT   El2=RA_MC_LUX
ITO RCN_A1     Add   El1=RA_MC_LUX   El2=C_1         Exit=RA_MC_SLOT
ITO RCN_W1     Write El1=RA_MC_SLOT   El2=Move
ITO RCN_A2     Add   El1=RA_MC_LUX   El2=C_2         Exit=RA_MC_SLOT
ITO RCN_W2     Write El1=RA_MC_SLOT   El2=RA_MA1
ITO RCN_A4     Add   El1=RA_MC_LUX   El2=C_4         Exit=RA_MC_SLOT
ITO RCN_W4     Write El1=RA_MC_SLOT   El2=RA_MC_DEST
ITO RCN_CAL    Voca  El1=AUTOLINK     Exit=RA_LINK
NOLINK
/Second ITO: Voca RA_MC_OP — via WAVE. Next-link from first via CHAIN.
CHAIN RCN_NLK
    Add   El1=RA_MC_LUX  El2=SLOT_NEXT  Exit=RA_MC_SLOT
    Write El1=RA_MC_SLOT  El2=RA_MC_J
WAVE RCN_J   Voca  El1=RA_MC_OP  Exit=RA_LINK_REF  At=RA_MC_J
ITO RCN_SL   Move  El1=RA_MC_J   Exit=RA_MC_PREV
RREDI RCN_RET
/── EMIT name arg → Move arg→RA_TW_LUX; Voca EMIT_STR_ENTRY; NOP
/MA0=name_addr MA1=arg_addr
NEWREF EMIT EMIT_SPN
NOLINK
ITO EMIT_SPN   Move  El1=RA_MA0          Exit=RA_MC_LUX
ITO EMIT_OP    Move  El1=EMIT_STR_ENTRY  Exit=RA_MC_OP
ITO EMIT_DST   Move  El1=RA_TW_LUX_REF  Exit=RA_MC_DEST
RVOCA EMIT_DO  RCN_IMPL
RREDI EMIT_RET
/── EMITI name arg → Move arg→RA_TMP2; Voca EMIT_INT_ENTRY; NOP
/MA0=name_addr MA1=arg_addr
NEWREF EMITI EMITI_SPN
NOLINK
ITO EMITI_SPN  Move  El1=RA_MA0          Exit=RA_MC_LUX
ITO EMITI_OP   Move  El1=EMIT_INT_ENTRY  Exit=RA_MC_OP
ITO EMITI_DST  Move  El1=RA_TMP2_REF     Exit=RA_MC_DEST
RVOCA EMITI_DO RCN_IMPL
RREDI EMITI_RET
/── PUTBYTE name arg → Move arg→RA_BYTE; Voca PUT_BYTE; NOP
/MA0=name_addr MA1=arg_addr
NEWREF PUTBYTE PUTBYTE_SPN
NOLINK
ITO PUTBYTE_SPN  Move  El1=RA_MA0       Exit=RA_MC_LUX
ITO PUTBYTE_OP   Move  El1=PUT_BYTE     Exit=RA_MC_OP
ITO PUTBYTE_DST  Move  El1=RA_BYTE_REF  Exit=RA_MC_DEST
RVOCA PUTBYTE_DO RCN_IMPL
RREDI PUTBYTE_RET
/── ITO: alloc 7 luces, self-ref, wire op/e1/e2/exit, autolink ──
/Called by bootstrap when command == "ITO"
/MA0=name_addr MA1=op_addr MA2=e1_addr MA3=e2_addr MA4=exit_addr
NEWREF ITO ITO_START
NOLINK
/Alloc 7 luces
ITO ITO_START       Move  El1=ITO_SIZE     Exit=RA_ALLOC_COUNT
RVOCA ITO_ALLOC     ALLOC_LUCES
/addr → RA_MC_LUX
ITO ITO_SETLUX     Move  El1=RA_ALLOC_RESULT Exit=RA_MC_LUX
/Register: aether[aether[MA0]] = addr (MA0 holds addr of name lux)
ITO ITO_REG         Write El1=RA_MA0       El2=RA_MC_LUX
/Self-ref: aether[addr] = addr
ITO ITO_SELF        Write El1=RA_MC_LUX   El2=RA_MC_LUX
/Write op at slot 1
ITO ITO_OPS         Add   El1=RA_MC_LUX   El2=C_1           Exit=RA_MC_SLOT
ITO ITO_OPW         Write El1=RA_MC_SLOT   El2=RA_MA1
/Write e1 at slot 2
ITO ITO_E1S         Add   El1=RA_MC_LUX   El2=C_2           Exit=RA_MC_SLOT
ITO ITO_E1W         Write El1=RA_MC_SLOT   El2=RA_MA2
/Write e2 at slot 3
ITO ITO_E2S         Add   El1=RA_MC_LUX   El2=C_3           Exit=RA_MC_SLOT
ITO ITO_E2W         Write El1=RA_MC_SLOT   El2=RA_MA3
/Write exit at slot 4
ITO ITO_EXS         Add   El1=RA_MC_LUX   El2=C_4           Exit=RA_MC_SLOT
ITO ITO_EXW         Write El1=RA_MC_SLOT   El2=RA_MA4
/Autolink: if RA_MC_PREV != 0, write addr into prev.next (slot 5)
ITO ITO_LCKZ        Equal El1=RA_MC_PREV   El2=C_0           Exit=RA_MC_SLOT
ITO ITO_LCKJ        JumpIf El1=RA_MC_SLOT  Exit=ITO_NOAUTO
ITO ITO_LADD        Add   El1=RA_MC_PREV   El2=SLOT_NEXT           Exit=RA_MC_SLOT
ITO ITO_LW          Write El1=RA_MC_SLOT   El2=RA_MC_LUX
ITO ITO_NOAUTO      Move  El1=RA_MC_LUX   Exit=RA_MC_PREV
RREDI ITO_RET
/── BLOCK: alloc N * 2 luces (raw pairs), register name ─────────
/MA0=name_addr MA1=count_addr (or literal count)
NEWREF BLOCK BLOCK_START
NOLINK
/count = aether[MA1] (it's a lux whose word = the count value)
ITO BLOCK_START     Read  El1=RA_MA1       Exit=RA_ALLOC_COUNT
/Multiply by 2 (each block entry = 2 luces)
ITO BLOCK_MUL       Mul   El1=RA_ALLOC_COUNT El2=C_2         Exit=RA_ALLOC_COUNT
RVOCA BLOCK_ALLOC   ALLOC_LUCES
/Register name → base addr
ITO BLOCK_REG       Write El1=RA_MA0       El2=RA_ALLOC_RESULT
RREDI BLOCK_RET
/── NEWSET: NEW + SET in one line (MA0=name_addr MA1=value_addr) ─
NEWREF NEWSET NEWSET_START
NOLINK
/Alloc 1 lux for name (already done in sweep A via lazy alloc)
/Just write the value into the name lux
ITO NEWSET_START    Write El1=RA_MA0       El2=RA_BS_PIVAL
RREDI NEWSET_RET
/── NEWREF: NEW + self-ref (or cross-ref) ───────────────────────
/MA0=name_addr MA1=ref_addr (0 if self-ref)
NEWREF NEWREF NEWREF_START
NOLINK
/If MA1 == 0: self-ref. Else: cross-ref to MA1.
ITO NEWREF_START    Equal El1=RA_MA1       El2=C_0           Exit=RA_MC_SLOT
ITO NEWREF_CKJ      JumpIf El1=RA_MC_SLOT  Exit=NEWREF_SELF
/Cross-ref: aether[aether[MA0]] = aether[MA1]
ITO NEWREF_CROSS    Read  El1=RA_MA1       Exit=RA_MC_SLOT
ITO NEWREF_CRW      Write El1=RA_MA0       El2=RA_MC_SLOT
RREDI NEWREF_RET
NOLINK
ITO NEWREF_SELF     Write El1=RA_MA0       El2=RA_MA0
RREDI NEWREF_SRET2
/── SETREF: SET word to addr of another symbol ──────────────────
/MA0=name_addr MA1=ref_addr
NEWREF SETREF SETREF_START
NOLINK
ITO SETREF_START    Write El1=RA_MA0       El2=RA_MA1
RREDI SETREF_RET
/── NOLINK: reset autolink chain (BS_LAST_ITO = 0) ─────────────
/MA0 unused
NEWREF NOLINK NOLINK_START
NOLINK
ITO NOLINK_START    Move  El1=C_0          Exit=RA_MC_PREV
RREDI NOLINK_RET
/============================================================
/SWITCH, FOR, SAVE — indentation-aware macros for saku.re
/
/These programs are called by saku.re during the unified pass.
/They have access to RA_LOAD_* registers (global file I/O state).
/
/When saku.re sees "SWITCH RA_FOO", it calls SWITCH with:
/  MA0 = addr of "SWITCH" symbol (the command)
/  MA1 = addr of register lux (RA_FOO)
/Then SWITCH reads following indented lines from RA_LOAD_FD itself.
/
/SWITCH MA1=reg_addr
/Reads indented lines until non-indented or EOF.
/Each line: "VAL DEST" or "V1 V2 > DEST"
/Emits: JEQ luces for each value.
/
/FOR MA1=el1 MA2=el2 MA3=el3 ...
/Reads indented body lines (one template).
/For each element: instantiates body with {X}=elem, {N}=index.
/
/SAVE MA1=reg1 MA2=reg2 ...
/Reads indented body lines.
/Emits: save-ITOs before body, restore-ITOs after.
/============================================================

/── Scratch registers for SWITCH/FOR/SAVE ────────────────────
NEW RA_SW_REG        /SWITCH: register being switched on
NEW RA_SW_VAL        /SWITCH: current value being compared
NEW RA_SW_DEST       /SWITCH: current destination label
NEW RA_SW_IDX        /SWITCH: JEQ counter
NEW RA_FOR_IDX       /FOR: current iteration index
NEW RA_FOR_ELEM      /FOR: current element addr
NEW RA_FOR_BODY      /FOR: addr of body template start
/FOR name buffer: stores element names as packed strings for {X} substitution
NEWSET FOR_IRIS_BUF_SIZE 2048
BLOCK  FOR_IRIS_BUF_000  2048  /name bytes storage
NEW    RA_FOR_IRIS_BUF   /base of name buffer
NEW    RA_FOR_IRIS_PTR   /write position in name buffer
NEW    RA_FOR_IRIS_LEN   /length of current element name in bytes
/Name index table: FOR_IRIS_IDX_000[i] = offset into name buf for element i
NEWSET FOR_IRIS_IDX_SIZE 8
BLOCK  FOR_IRIS_IDX_000  8     /offsets for up to 7 elements
NEW    RA_FOR_IRIS_IDX_BASE    /base of name index table
NEW RA_SV_REGS       /SAVE: packed register list
NEW RA_SV_COUNT      /SAVE: number of registers to save

/── SWITCH: emit JEQ luces for each indented val→dest line ──
/MA1 = register lux addr to switch on.
/Line formats:
/  "VAL DEST"         → single value match
/  "V1 V2 > DEST"     → multiple values to same dest (FOR-like)
/  "V1 V2 V3 > DEST"  → three values to same dest
/Reads indented lines until non-indented or EOF.
NEWREF SWITCH SWITCH_START
NEW RA_SW_AROW   /addr of '>' token (detected by byte 62)
NEW RA_SW_BASE   /SWITCH NAME: base name addr (MA2), 0 if anonymous
NEWSET SW_ARROW_BYTE 62   /'>'
NOLINK
ITO SWITCH_START     Move  El1=RA_MA1       Exit=RA_SW_REG
ITO SWITCH_IDXINIT   Move  El1=C_0          Exit=RA_SW_IDX
/MA2 = optional block name (SWITCH reg NAME); 0 if anonymous
ITO SWITCH_SAVBASE   Move  El1=RA_MA2       Exit=RA_SW_BASE
RVOCA SWITCH_RDLINE  LOAD_READ_LINE
/SWITCH_RDLINE is the re-entry point after EVERY case line (all of
/SWITCH_E1_JMP..SWITCH_E4_JMP and SWITCH_T1_RETRY jump here), not just
/before the first one. LOAD_READ_LINE + the EOF/indent checks below MUST
/run on every iteration, exactly like loader.py's Python 'switch' mode
/re-checks indentation on every raw line it reads (_expand_indent). A
/loop-back straight to SWITCH_LINE (skipping this) would blindly token-read
/whatever follows regardless of indentation -- so a dedented line right
/after the body (e.g. a "── divider ──" comment that's missing its comment
/delimiter) gets misread as another case line instead of ending the SWITCH.
/EOF or non-indented → done
ITO SWITCH_EOFCK     Equal El1=RA_LOAD_BYTE  El2=C_0          Exit=RA_BS_FLAG
ITO SWITCH_EOFJ      JumpIf El1=RA_BS_FLAG   Exit=SWITCH_DONE
ITO SWITCH_INDCK     Equal El1=SK_IND_DEPTH El2=C_0        Exit=RA_BS_FLAG
ITO SWITCH_INDJ      JumpIf El1=RA_BS_FLAG   Exit=SWITCH_DONE
/Read tokens: collect values until '>' or end of line, then dest
ITO SWITCH_VCLR0     Move  El1=C_0          Exit=RA_MA2
ITO SWITCH_VCLR1     Move  El1=C_0          Exit=RA_MA3
ITO SWITCH_VCLR2     Move  El1=C_0          Exit=RA_MA4
ITO SWITCH_VCLR3     Move  El1=C_0          Exit=RA_MA5
ITO SWITCH_VCNT      Move  El1=C_0          Exit=RA_BS_ELC
/Read first token (value or arrow)
RVOCA SWITCH_LINE   LOAD_READ_TOKEN
ITO SWITCH_T1EK      Equal El1=RA_LOAD_TLEN  El2=C_0          Exit=RA_BS_FLAG
ITO SWITCH_T1EJ      JumpIf El1=RA_BS_FLAG   Exit=SWITCH_T1_EOFCK
RVOCA SWITCH_T1I     SWITCH_RESOLVE_VALUE
ITO SWITCH_T1SV      Move  El1=RA_LOAD_RESULT Exit=RA_MA2
/Peek: is second token '>' or another val or dest?
RVOCA SWITCH_T2      LOAD_READ_TOKEN
ITO SWITCH_T2EK      Equal El1=RA_LOAD_TLEN  El2=C_0          Exit=RA_BS_FLAG
ITO SWITCH_T2EJ      JumpIf El1=RA_BS_FLAG   Exit=SWITCH_EMIT1
/Check if this token is '>'
ITO SWITCH_T2B0      Read  El1=BS_TOKBUF_BASE Exit=RA_BS_TMP3
ITO SWITCH_T2ARR     Equal El1=RA_BS_TMP3    El2=SW_ARROW_BYTE Exit=RA_BS_FLAG
ITO SWITCH_T2ARRJ    JumpIf El1=RA_BS_FLAG   Exit=SWITCH_READ_DEST_AFTER_1
RVOCA SWITCH_T2I     SWITCH_RESOLVE_VALUE
ITO SWITCH_T2SV      Move  El1=RA_LOAD_RESULT Exit=RA_MA3
/Read third token
RVOCA SWITCH_T3      LOAD_READ_TOKEN
ITO SWITCH_T3EK      Equal El1=RA_LOAD_TLEN  El2=C_0          Exit=RA_BS_FLAG
ITO SWITCH_T3EJ      JumpIf El1=RA_BS_FLAG   Exit=SWITCH_T2_NOARROW
ITO SWITCH_T3B0      Read  El1=BS_TOKBUF_BASE Exit=RA_BS_TMP3
ITO SWITCH_T3ARR     Equal El1=RA_BS_TMP3    El2=SW_ARROW_BYTE Exit=RA_BS_FLAG
ITO SWITCH_T3ARRJ    JumpIf El1=RA_BS_FLAG   Exit=SWITCH_READ_DEST_AFTER_2
RVOCA SWITCH_T3I     SWITCH_RESOLVE_VALUE
ITO SWITCH_T3SV      Move  El1=RA_LOAD_RESULT Exit=RA_MA4
/Read fourth token (should be '>' or dest)
RVOCA SWITCH_T4      LOAD_READ_TOKEN
ITO SWITCH_T4EK      Equal El1=RA_LOAD_TLEN  El2=C_0          Exit=RA_BS_FLAG
ITO SWITCH_T4EJ      JumpIf El1=RA_BS_FLAG   Exit=SWITCH_EMIT3
ITO SWITCH_T4B0      Read  El1=BS_TOKBUF_BASE Exit=RA_BS_TMP3
ITO SWITCH_T4ARR     Equal El1=RA_BS_TMP3    El2=SW_ARROW_BYTE Exit=RA_BS_FLAG
ITO SWITCH_T4ARRJ    JumpIf El1=RA_BS_FLAG   Exit=SWITCH_READ_DEST_AFTER_3
/Four values: MA2/MA3/MA4/MA5
RVOCA SWITCH_T4I     SWITCH_RESOLVE_VALUE
ITO SWITCH_T4SV      Move  El1=RA_LOAD_RESULT Exit=RA_MA5
RVOCA SWITCH_T5      LOAD_READ_TOKEN
RVOCA SWITCH_T5I     LOAD_INTERN_TOKEN
ITO SWITCH_T5SD      Move  El1=RA_LOAD_RESULT Exit=RA_SW_DEST
ITO SWITCH_EMIT4     Jump  Exit=SWITCH_DO_EMIT4
/Read dest after 1 val (prev token was '>')
NOLINK
RVOCA SWITCH_READ_DEST_AFTER_1   LOAD_READ_TOKEN
RVOCA SWITCH_RDA1_I  LOAD_INTERN_TOKEN
ITO SWITCH_RDA1_SD   Move  El1=RA_LOAD_RESULT Exit=RA_SW_DEST
ITO SWITCH_RDA1_JMP  Jump  Exit=SWITCH_EMIT1
/Read dest after 2 vals
NOLINK
RVOCA SWITCH_READ_DEST_AFTER_2   LOAD_READ_TOKEN
RVOCA SWITCH_RDA2_I  LOAD_INTERN_TOKEN
ITO SWITCH_RDA2_SD   Move  El1=RA_LOAD_RESULT Exit=RA_SW_DEST
ITO SWITCH_RDA2_JMP  Jump  Exit=SWITCH_EMIT2
/Read dest after 3 vals
NOLINK
RVOCA SWITCH_READ_DEST_AFTER_3   LOAD_READ_TOKEN
RVOCA SWITCH_RDA3_I  LOAD_INTERN_TOKEN
ITO SWITCH_RDA3_SD   Move  El1=RA_LOAD_RESULT Exit=RA_SW_DEST
ITO SWITCH_RDA3_JMP  Jump  Exit=SWITCH_EMIT3
/No-arrow 2-token form: "val dest" (e.g. "C_1 ETH_V1", no '>'). T2 was
/already resolved via SWITCH_T2I above (RA_MA3 = its address) on the
/assumption it might be a second value -- it wasn't, T3 coming back empty
/confirms this line has exactly 2 tokens, so T2 IS the destination per the
/canonical rule (loader.py's Python SWITCH expansion: no arrow -> exactly
/2 tokens, value then dest -- see _expand_indent's switch-mode handling).
/Land just past EMIT1's own dest-move (which reads RA_LOAD_RESULT, stale
/here after the SWITCH_T3 read) with RA_SW_DEST already set from RA_MA3.
NOLINK
ITO SWITCH_T2_NOARROW Move  El1=RA_MA3        Exit=RA_SW_DEST
ITO SWITCH_T2_NA_JMP  Jump                    Exit=SWITCH_E1_SKL
/Emit JEQ for 1 val: MA2=val, SW_DEST
NOLINK
ITO SWITCH_EMIT1     Move  El1=RA_LOAD_RESULT Exit=RA_SW_DEST
RVOCA SWITCH_E1_SKL  BS_SKIP_TO_EOL
RVOCA SWITCH_E1_MA0 SWITCH_RESOLVE_NAME
ITO SWITCH_E1_MA1    Move  El1=RA_SW_REG    Exit=RA_MA1
ITO SWITCH_E1_MA3    Move  El1=RA_SW_DEST   Exit=RA_MA3
/Alloc _J (JumpIf) and _K (NOP) luces for JEQ macro (MA4/MA5 must be non-zero)
ITO SWITCH_E1_AC4C   Move  El1=ITO_SIZE     Exit=RA_ALLOC_COUNT
RVOCA SWITCH_E1_AC4  ALLOC_LUCES
ITO SWITCH_E1_MA4    Move  El1=RA_ALLOC_RESULT Exit=RA_MA4
ITO SWITCH_E1_AC5C   Move  El1=ITO_SIZE     Exit=RA_ALLOC_COUNT
RVOCA SWITCH_E1_AC5  ALLOC_LUCES
ITO SWITCH_E1_MA5    Move  El1=RA_ALLOC_RESULT Exit=RA_MA5
RVOCA SWITCH_E1_JEQ  JEQ
ITO SWITCH_E1_INC    Add   El1=RA_SW_IDX   El2=C_1            Exit=RA_SW_IDX
ITO SWITCH_E1_JMP    Jump  Exit=SWITCH_RDLINE
/Emit JEQ for 2 vals: MA2, MA3 to same dest
NOLINK
RVOCA SWITCH_EMIT2   BS_SKIP_TO_EOL
RVOCA SWITCH_E2_MA0 SWITCH_RESOLVE_NAME
ITO SWITCH_E2_MA1    Move  El1=RA_SW_REG    Exit=RA_MA1
ITO SWITCH_E2_MA3D   Move  El1=RA_SW_DEST   Exit=RA_MA3
RVOCA SWITCH_E2_J1   JEQ
ITO SWITCH_E2_INC1   Add   El1=RA_SW_IDX   El2=C_1            Exit=RA_SW_IDX
RVOCA SWITCH_E2_MA0B SWITCH_RESOLVE_NAME
ITO SWITCH_E2_MA2B   Move  El1=RA_MA3       Exit=RA_MA2
ITO SWITCH_E2_MA3B   Move  El1=RA_SW_DEST   Exit=RA_MA3
RVOCA SWITCH_E2_J2   JEQ
ITO SWITCH_E2_INC2   Add   El1=RA_SW_IDX   El2=C_1            Exit=RA_SW_IDX
ITO SWITCH_E2_JMP    Jump  Exit=SWITCH_RDLINE
/Emit JEQ for 3 vals
NOLINK
RVOCA SWITCH_EMIT3   BS_SKIP_TO_EOL
RVOCA SWITCH_E3_MA0 SWITCH_RESOLVE_NAME
ITO SWITCH_E3_MA1    Move  El1=RA_SW_REG    Exit=RA_MA1
ITO SWITCH_E3_MA3D   Move  El1=RA_SW_DEST   Exit=RA_MA3
RVOCA SWITCH_E3_J1   JEQ
ITO SWITCH_E3_INC1   Add   El1=RA_SW_IDX   El2=C_1            Exit=RA_SW_IDX
RVOCA SWITCH_E3_MA0B SWITCH_RESOLVE_NAME
ITO SWITCH_E3_MA2B   Move  El1=RA_MA3       Exit=RA_MA2
RVOCA SWITCH_E3_J2   JEQ
ITO SWITCH_E3_INC2   Add   El1=RA_SW_IDX   El2=C_1            Exit=RA_SW_IDX
RVOCA SWITCH_E3_MA0C SWITCH_RESOLVE_NAME
ITO SWITCH_E3_MA2C   Move  El1=RA_MA4       Exit=RA_MA2
RVOCA SWITCH_E3_J3   JEQ
ITO SWITCH_E3_INC3   Add   El1=RA_SW_IDX   El2=C_1            Exit=RA_SW_IDX
ITO SWITCH_E3_JMP    Jump  Exit=SWITCH_RDLINE
/Emit JEQ for 4 vals: MA2/MA3/MA4/MA5 → same dest
NOLINK
RVOCA SWITCH_DO_EMIT4   BS_SKIP_TO_EOL
RVOCA SWITCH_E4_MA0 SWITCH_RESOLVE_NAME
ITO SWITCH_E4_MA1    Move  El1=RA_SW_REG    Exit=RA_MA1
ITO SWITCH_E4_MA3D   Move  El1=RA_SW_DEST   Exit=RA_MA3
RVOCA SWITCH_E4_J1   JEQ
ITO SWITCH_E4_INC1   Add   El1=RA_SW_IDX   El2=C_1            Exit=RA_SW_IDX
RVOCA SWITCH_E4_MA0B SWITCH_RESOLVE_NAME
ITO SWITCH_E4_MA2B   Move  El1=RA_MA3       Exit=RA_MA2
RVOCA SWITCH_E4_J2   JEQ
ITO SWITCH_E4_INC2   Add   El1=RA_SW_IDX   El2=C_1            Exit=RA_SW_IDX
RVOCA SWITCH_E4_MA0C SWITCH_RESOLVE_NAME
ITO SWITCH_E4_MA2C   Move  El1=RA_MA4       Exit=RA_MA2
RVOCA SWITCH_E4_J3   JEQ
ITO SWITCH_E4_INC3   Add   El1=RA_SW_IDX   El2=C_1            Exit=RA_SW_IDX
RVOCA SWITCH_E4_MA0D SWITCH_RESOLVE_NAME
ITO SWITCH_E4_MA2D   Move  El1=RA_MA5       Exit=RA_MA2
RVOCA SWITCH_E4_J4   JEQ
ITO SWITCH_E4_INC4   Add   El1=RA_SW_IDX   El2=C_1            Exit=RA_SW_IDX
ITO SWITCH_E4_JMP    Jump  Exit=SWITCH_RDLINE
NOLINK
/SWITCH_T1_EOFCK: SWITCH_T1EJ lands here when the first token of a case
/line came back empty (TLEN==0). That's ambiguous on its own -- it happens
/both for a harmless blank-line boundary (LOAD_READ_TOKEN crossed into a
/new line with nothing read yet -- retry is correct) AND for genuine EOF
/(the file is exhausted -- RA_LOAD_BYTE==0 confirms this). Retrying
/unconditionally on the EOF case is an infinite loop: LOAD_READ_TOKEN keeps
/hitting BS_READ_BYTE, which keeps re-confirming EOF, which keeps producing
/another empty token, forever. Checking RA_LOAD_BYTE here disambiguates the
/two cases and was the missing piece -- SWITCH_RDLINE's own EOF/dedent
/checks (SWITCH_EOFCK/SWITCH_INDCK) only run once per *line*, not on every
/inner per-token retry, so they never catch this.
ITO SWITCH_T1_EOFCK  Equal El1=RA_LOAD_BYTE  El2=C_0           Exit=RA_BS_FLAG
ITO SWITCH_T1_EOFCJ  JumpIf El1=RA_BS_FLAG   Exit=SWITCH_DONE
ITO SWITCH_T1_RETRY  Jump                    Exit=SWITCH_RDLINE
NOLINK
/Update RA_MC_PREV = last _K NOP (in RA_MA5 after last JEQ call) so autolink works
ITO SWITCH_DONE      Move  El1=RA_MA5        Exit=RA_MC_PREV
RREDI SWITCH_RET
/── SWITCH_RESOLVE_NAME: set MA0 for next JEQ call ──────────────────────────
/If RA_SW_BASE=0 → anonymous: MA0 = RA_SW_IDX (numeric, JEQ creates __sw_N)
/If RA_SW_BASE≠0 → named: build IRIS_N lux, put its addr in MA0
/Leaf.
NEWREF SWITCH_RESOLVE_NAME SW_RN_IMPL
NOLINK
JZ SW_RN_IMPL RA_SW_BASE SW_RN_ANON_PATH
/Named: build BASE_N packed name, intern it → addr in MA0
/Use CHAIN_WRITE_NUM machinery via LD_NI_OUT/LD_CH_IDX
ITO SW_RN_INIT   Move  El1=BS_TOKBUF_BASE  Exit=LD_NI_OUT
ITO SW_RN_BASE   Move  El1=RA_SW_BASE      Exit=LD_CH_BASE
RVOCA SW_RN_WP   CHAIN_WRITE_PACKED
WRITE_OUT SW_RN_U UNDERSCORE
ITO SW_RN_SETIDX Move  El1=RA_SW_IDX      Exit=LD_CH_IDX
RVOCA SW_RN_WN   CHAIN_WRITE_NUM
ITO SW_RN_NUL    Write El1=LD_NI_OUT      El2=C_0
RVOCA SW_RN_INT  LOAD_INTERN_TOKEN
ITO SW_RN_SETMA0 Move  El1=RA_LOAD_RESULT Exit=RA_MA0
RREDI SW_RN_RET
/Anonymous: MA0 = RA_SW_IDX (numeric counter)
NOLINK
ITO SW_RN_ANON_PATH Move El1=RA_SW_IDX    Exit=RA_MA0
RREDI SW_RN_ANON_RET
/── SWITCH_RESOLVE_VALUE: resolve a case-value token to a comparable addr ──
/Drop-in replacement for "RVOCA ... LOAD_INTERN_TOKEN" when reading a SWITCH
/case value. A bare numeric literal (e.g. "0", "-1") must compare against
/its actual VALUE, not get interned as an arbitrary identifier string —
/interning "0" gives some unrelated lux address, so "Equal byte, 0" would
/never match a genuine zero byte. Named cases (SP, TAB, LF, CR, SLASH, ...)
/keep the existing identifier-interning behavior unchanged.
/IN:  BS_TOKBUF_BASE[0..RA_LOAD_TLEN-1] = case-value token
/OUT: RA_LOAD_RESULT = address whose word holds the comparison value
/     (a fresh 1-lux register for numeric literals, the interned identifier
/      lux otherwise) — identical output contract to LOAD_INTERN_TOKEN.
NOLINK
NEWREF SWITCH_RESOLVE_VALUE SW_RV_B0
ITO SW_RV_B0      Read   El1=BS_TOKBUF_BASE Exit=RA_BS_TMP
ITO SW_RV_DGE     ULess  El1=RA_BS_TMP      El2=ASCII_0     Exit=RA_BS_FLAG
ITO SW_RV_DGEJ    JumpIf El1=RA_BS_FLAG     Exit=SW_RV_MNCK
ITO SW_RV_DGL     ULess  El1=ASCII_9        El2=RA_BS_TMP   Exit=RA_BS_FLAG
ITO SW_RV_DGLJ    JumpIf El1=RA_BS_FLAG     Exit=SW_RV_MNCK
ITO SW_RV_ISNUM   Jump   Exit=SW_RV_NUMERIC
NOLINK
ITO SW_RV_MNCK    Equal  El1=RA_BS_TMP      El2=MINUS       Exit=RA_BS_FLAG
ITO SW_RV_MNCKJ   JumpIf El1=RA_BS_FLAG     Exit=SW_RV_NUMERIC
/Not numeric: identical to the old behavior (intern as identifier)
RVOCA SW_RV_NAME  BS_INTERN
ITO SW_RV_NSV     Move   El1=RA_BS_RESULT   Exit=RA_LOAD_RESULT
RREDI SW_RV_NRET
/Numeric literal: parse value into a fresh 1-lux scratch register
NOLINK
ITO SW_RV_NUMERIC Move   El1=C_1            Exit=RA_ALLOC_COUNT
RVOCA SW_RV_ALLOC ALLOC_LUCES
RVOCA SW_RV_PARSE BS_PARSE_INT
ITO SW_RV_WRITE   Write  El1=RA_ALLOC_RESULT El2=RA_BS_PIVAL
ITO SW_RV_SETR    Move   El1=RA_ALLOC_RESULT Exit=RA_LOAD_RESULT
RREDI SW_RV_VRET
/── FOR: iterate over MA1..MA7 elements, expand body template ─
/MA1..MA7 = element addrs (0 = end of list).
/Reads indented body lines as template.
/Instantiates body for each element: {X}→elem, {N}→index.
NEWREF FOR FOR_START
NOLINK
ITO FOR_START        Move  El1=C_0          Exit=RA_FOR_IDX
/Read indented body lines into template buffer
RVOCA FOR_RDBODY     LOAD_READ_BODY
/Iterate: MA1..MA7
ITO FOR_ELEM1        Move  El1=RA_MA1       Exit=RA_FOR_ELEM
RVOCA FOR_ITER       FOR_ITER_ELEM
ITO FOR_ELEM2        Move  El1=RA_MA2       Exit=RA_FOR_ELEM
ITO FOR_ELEM2CK      Equal El1=RA_FOR_ELEM  El2=C_0           Exit=RA_BS_FLAG
ITO FOR_ELEM2CJ      JumpIf El1=RA_BS_FLAG  Exit=FOR_DONE
RVOCA FOR_ITER2      FOR_ITER_ELEM
ITO FOR_ELEM3        Move  El1=RA_MA3       Exit=RA_FOR_ELEM
ITO FOR_ELEM3CK      Equal El1=RA_FOR_ELEM  El2=C_0           Exit=RA_BS_FLAG
ITO FOR_ELEM3CJ      JumpIf El1=RA_BS_FLAG  Exit=FOR_DONE
RVOCA FOR_ITER3      FOR_ITER_ELEM
ITO FOR_ELEM4        Move  El1=RA_MA4       Exit=RA_FOR_ELEM
ITO FOR_ELEM4CK      Equal El1=RA_FOR_ELEM  El2=C_0           Exit=RA_BS_FLAG
ITO FOR_ELEM4CJ      JumpIf El1=RA_BS_FLAG  Exit=FOR_DONE
RVOCA FOR_ITER4      FOR_ITER_ELEM
ITO FOR_ELEM5        Move  El1=RA_MA5       Exit=RA_FOR_ELEM
ITO FOR_ELEM5CK      Equal El1=RA_FOR_ELEM  El2=C_0           Exit=RA_BS_FLAG
ITO FOR_ELEM5CJ      JumpIf El1=RA_BS_FLAG  Exit=FOR_DONE
RVOCA FOR_ITER5      FOR_ITER_ELEM
ITO FOR_ELEM6        Move  El1=RA_MA6       Exit=RA_FOR_ELEM
ITO FOR_ELEM6CK      Equal El1=RA_FOR_ELEM  El2=C_0           Exit=RA_BS_FLAG
ITO FOR_ELEM6CJ      JumpIf El1=RA_BS_FLAG  Exit=FOR_DONE
RVOCA FOR_ITER6      FOR_ITER_ELEM
ITO FOR_ELEM7        Move  El1=RA_MA7       Exit=RA_FOR_ELEM
ITO FOR_ELEM7CK      Equal El1=RA_FOR_ELEM  El2=C_0           Exit=RA_BS_FLAG
ITO FOR_ELEM7CJ      JumpIf El1=RA_BS_FLAG  Exit=FOR_DONE
RVOCA FOR_ITER7      FOR_ITER_ELEM
RREDI FOR_DONE
/── FOR_ITER_ELEM: expand body template for current element ───
/RA_FOR_ELEM = current element addr, RA_FOR_IDX = iteration index.
/Walks body template, substitutes {X}→RA_FOR_ELEM, {N}→RA_FOR_IDX.
/Then processes the resulting line through the loader dispatch.
NEWREF FOR_ITER_ELEM FOR_IE_START
NOLINK
/Walk template buffer, substitute placeholders, emit lines
RVOCA FOR_IE_START   LOAD_EXPAND_TEMPLATE
ITO FOR_IE_INC       Add   El1=RA_FOR_IDX   El2=C_1           Exit=RA_FOR_IDX
RREDI FOR_IE_RET
/── SAVE: emit save/restore ITOs around body ─────────────────
/MA1..MA7 = register addrs to save/restore.
/RA_LINK must NEVER be passed here -- it is preserved automatically by the
/call stack (Voca/Redi push/pop on RA_SP) at any nesting depth, for every
/early-return point a body has, at zero per-call cost. A manual save here
/would just be a second, redundant, bug-prone reimplementation of the same
/mechanism (see HISTORY.md). The Python-level SAVE preprocessor enforces
/this by stripping RA_LINK unconditionally; this Reca-level macro relies on
/callers following the same rule.
/Reads indented body lines and processes them.
/Emits: Move reg→S_reg before body, Move S_reg→reg after.
NEWREF SAVE SAVE_START
NOLINK
/Emit save-ITOs for each MA1..MA7 register
/Each: ITO __sv_N Move El1=REG Exit=S_REG
RVOCA SAVE_START     SAVE_EMIT_SAVES
/Read and process body lines
RVOCA SAVE_BODY      LOAD_READ_BODY
RVOCA SAVE_DOBODY    LOAD_PROCESS_BODY
/Emit restore-ITOs
RVOCA SAVE_RESTORE   SAVE_EMIT_RESTORES
RREDI SAVE_RET
/── LOAD_*/SAVE_EMIT_* subroutines are implemented in saku.re. No forward-
/declaration stub needed here: Wave A allocates every NEW/NEWREF/NEWSET/
/ITO/BLOCK name from every file before Wave B wires anything (see
/auto_discover's docstring: "Two-phase makes ordering irrelevant... Forward
/references resolved automatically"), so saku.re's real implementations
/are already addressable by the time macros.re's RVOCA calls reference
/them by name, regardless of file processing order. A stub-then-overwrite
/indirection used to sit here; it was redundant with this guarantee and,
/worse, never actually got correctly overwritten in the final build (see
/BUGS.md) -- every one of its 8 names ended up permanently stuck pointing
/at its own no-op stub instead of the real saku.re implementation.
/============================================================
/── NEXO: build a packed string template and optionally link to Yaku ──────────
/NEXO name [relation] [alias]
/  body lines (indented)...
/
/  name     — lux to store the packed string in (MA0)
/  relation — optional: if given, LINK Yaku_name --relation--> Yaku  (MA1, 0=absent)
/  alias    — optional: if given, also LINK Yaku_name --ForType--> alias (MA2, 0=absent)
/
/Replaces: YAKU_NEXO, YAKU_NEXO_TERM, YAKU_NEXO_CMP, YAKU_NEXO_ARITH, YAKU_NEXO_ALIAS.
/Examples:
/  NEXO Add HasArithResult     ← was YAKU_NEXO_ARITH Add
/  NEXO End Terminates         ← was YAKU_NEXO_TERM End
/  NEXO Equal HasCmpResult     ← was YAKU_NEXO_CMP Equal
/  NEXO Read Terminates Write  ← was YAKU_NEXO_ALIAS Read (alias=Write)
/  NEXO Move                   ← was YAKU_NEXO Move (no relation, inline or body)
NEWREF NEXO NEXO_START
NOLINK
/Init packed string builder
CHAIN NEXO_START
    Move  El1=C_0          Exit=PGB_PS_FIRST
    Move  El1=C_0          Exit=PGB_PS_WORD
    Move  El1=C_0          Exit=PGB_PS_SHIFT
RVOCA SN_SKP         BS_SKIP_TO_EOL
/Read body lines until non-indented or EOF
RVOCA SN_LINE_LOOP   LOAD_READ_LINE
/EOF or non-indented → done
JEQ SN_EOFCK   RA_LOAD_BYTE C_0 SN_DONE_BUILD
JEQ SN_INDCK   SK_IND_DEPTH C_0 SN_DONE_BUILD
/Emit LF between lines (except first)
JZ SN_LFCK     PGB_PS_FIRST SN_READ_LINE_BYTES
ITO SN_LF_EMIT Move  El1=LF  Exit=RA_TMP
RVOCA SN_LFEB  PGB_EMIT_BYTE
/Read and emit bytes of this line
RVOCA SN_READ_LINE_BYTES   BS_READ_BYTE
JEQ SN_BEOF    RA_LOAD_BYTE C_0  SN_LINE_LOOP
JEQ SN_BLF     RA_LOAD_BYTE LF   SN_LINE_LOOP
/Check for '{'
JEQ SN_LBCK    RA_LOAD_BYTE LBRACE SN_PLACEHOLDER
/Ordinary byte
ITO SN_EBYTE   Move  El1=RA_LOAD_BYTE  Exit=RA_TMP
RVOCA SN_EB    PGB_EMIT_BYTE
ITO SN_BJMP    Jump  Exit=SN_READ_LINE_BYTES
/Placeholder: read name bytes into tokbuf, call PGB_PLACEHOLDER
CHAIN SN_PLACEHOLDER
    Move  El1=C_0             Exit=PR_LPOS
    Move  El1=BS_TOKBUF_BASE  Exit=PR_LBUF
RVOCA SN_PL_RBYTE      BS_READ_BYTE
JEQ SN_PL_RBCK         RA_LOAD_BYTE RBRACE SN_PL_CALL
ITO SN_PL_STORE  Add   El1=BS_TOKBUF_BASE El2=PR_LPOS  Exit=RA_BS_TMP
ITO SN_PL_SW     Write El1=RA_BS_TMP      El2=RA_LOAD_BYTE
ITO SN_PL_INC    Add   El1=PR_LPOS        El2=C_1  Exit=PR_LPOS
ITO SN_PL_JMP    Jump  Exit=SN_PL_RBYTE
ITO SN_PL_CALL   Move  El1=PR_LPOS        Exit=PR_LLEN
RVOCA SN_PL_PHCALL  PGB_PLACEHOLDER
ITO SN_PL_BJMP   Jump  Exit=SN_READ_LINE_BYTES
/Done: finalize + write packed string into MA0.word
NOLINK
RVOCA SN_DONE_BUILD   PGB_FINALIZE
ITO SN_WRITE     Write El1=RA_MA0  El2=PGB_PS_FIRST
/If MA1 (relation) given: LINK MA0 --MA1--> Yaku
JZ SN_RELCK      RA_MA1  SN_ALIAS_CK
CHAIN SN_REL_SRC
    Move  El1=RA_MA0  Exit=RA_LM_SRC
    Move  El1=RA_MA1  Exit=RA_LM_REL
    Move  El1=Yaku    Exit=RA_LM_EXIT
RVOCA SN_REL_ADD  ADD_LUMEN
/If MA2 (alias) given: LINK MA0 --ForType--> MA2
NOLINK
JZ SN_ALIAS_CK   RA_MA2  SN_NEXO_DONE
CHAIN SN_ALIAS_SRC
    Move  El1=RA_MA0   Exit=RA_LM_SRC
    Move  El1=ForType  Exit=RA_LM_REL
    Move  El1=RA_MA2   Exit=RA_LM_EXIT
RVOCA SN_ALIAS_ADD  ADD_LUMEN
NOLINK
RREDI SN_NEXO_DONE

/── FUNC/ENDFUNC: REMOVED ──────────────────────────────────────────────────
/These macros generated the old manual RA_LINK save/restore pattern
/(push via CS_PUSH, pop via CS_POP). Zero callers remain in the project —
/the automatic call stack (Voca/Redi push/pop RA_LINK on RA_SP) makes this
/pattern fully redundant. See runtime/registers.re for the current convention.

/── CHAIN: single NOLINK then chain of luces ──────────────────────────────────
/CHAIN           → NOLINK + luces with explicit names (no auto-naming)
/CHAIN NAME      → NOLINK + luces with auto-naming IRIS_SUFFIX_N
/
/Without block name (MA0=0):
/    NAME1 op args   → ITO NAME1 op args
/    NAME2 op args   → ITO NAME2 op args
/    RVOCA X Y       → RVOCA (sub-lux, indent=2)
/
/With block name (MA0=name_addr):
/    _DONE op args  → ITO IRIS_DONE_N op args  (underscore prefix = add to name)
/    op args        → ITO IRIS_N op args        (no underscore = op directly)
/    RVOCA X Y      → RVOCA (sub-lux, indent=2)
/
/MA0=block_name (0 if anonymous)
NEWREF CHAIN CHAIN_START
NEW LD_NI_PTR        /walk pointer in body buffer
NEW LD_NI_MARK       /current marker byte
NEW LD_CH_IDX        /CHAIN: auto-name counter (0-based)
NEW LD_CH_BASE       /CHAIN: base name addr (MA0)
NEW LD_CH_FULLY_ANON /CHAIN _: 1 if fully anonymous (__ch_N naming), 0 otherwise
NEW LD_CH_NOLINK_MOD /CHAIN | NOLINK: 1 if the NOLINK modifier is present
NEW LD_CH_ANON_MOD   /CHAIN | ANON: 1 if the ANON modifier is present
NEW LD_CH_OWN_NAME   /preserved original MA0, even when ANON clears LD_CH_BASE
NEW LD_CH_ALIASED    /1 once NAME has been ALIASed to the first generated item
NEW LD_CH_LINKOVR    /1 if the current item's body line had a leading '-'
                       /marker (suppresses this item's per-item NOLINK)
NEW ANON              /CHAIN | ANON modifier marker -- comparison target only,
                       /no functional behavior of its own (unlike NOLINK)
/CHAIN_MOD_NOLINK_REF/CHAIN_MOD_ANON_REF: comparison targets for modifier
/detection. NOLINK is a real, functional macro -- its word field holds
/NOLINK_START's address, not its own address -- so comparing a resolved
/MA-slot value (which holds NOLINK's *own* address, from runtime
/interning) directly against "NOLINK" via Equal (which always
/dereferences both sides) would incorrectly compare against
/NOLINK_START instead. NEWREF X Y sets word(X)=addr(Y) (not word(Y)),
/giving a correct comparison target for both modifiers uniformly.
NEWREF CHAIN_MOD_NOLINK_REF NOLINK
NEWREF CHAIN_MOD_ANON_REF   ANON
NOLINK
/Single NOLINK: clear RA_MC_PREV
CLEAR CHAIN_START        RA_MC_PREV
CLEAR CHAIN_IDX          LD_CH_IDX
CLEAR CHAIN_FANON        LD_CH_FULLY_ANON
CLEAR CHAIN_NLMODCL      LD_CH_NOLINK_MOD
CLEAR CHAIN_ANMODCL      LD_CH_ANON_MOD
CLEAR CHAIN_ALSCL        LD_CH_ALIASED
ITO CHAIN_SAVBASE        Move  El1=RA_MA0      Exit=LD_CH_BASE
ITO CHAIN_SAVOWN         Move  El1=RA_MA0      Exit=LD_CH_OWN_NAME
/Check if CHAIN _ (MA0 = lux for "_"): detect by reading packed word = 0x5F (95)
JZ CHAIN_BASENIL LD_CH_BASE CHAIN_MODCK
ITO CHAIN_BASEWORD       Read  El1=LD_CH_BASE  Exit=LD_NI_MARK
JEQ CHAIN_FANON_CK       LD_NI_MARK UNDERSCORE CHAIN_SET_FANON
ITO CHAIN_FANON_SKIP     Jump  Exit=CHAIN_MODCK
NOLINK
ITO CHAIN_SET_FANON      Move  El1=C_1         Exit=LD_CH_FULLY_ANON
/Modifier check: "CHAIN [name] | MOD MOD ..." -- MA1=='|' signals modifiers
/follow in MA2..MA7. Detected the same way "_" is: read the resolved
/token's packed word directly (a 1-byte name's packed string IS its own
/word value), no separate interning needed for the '|' separator itself.
NOLINK
JZ CHAIN_MODCK    RA_MA1 CHAIN_RDBODY
ITO CHAIN_PIPEPK         Read  El1=RA_MA1     Exit=LD_NI_MARK
JEQ CHAIN_PIPECK         LD_NI_MARK ASCII_PIPE CHAIN_MODSCAN0
ITO CHAIN_PIPESKP        Jump  Exit=CHAIN_RDBODY
/Scan MA2..MA7 for NOLINK / ANON (order-independent, non-branching:
/each slot either matches one modifier or contributes 0 -- accumulate
/via Add rather than branch-and-converge, since at most one of NOLINK/
/ANON can match a given slot and unused slots are simply 0/no-match).
NOLINK
FOR RA_MA2 RA_MA3 RA_MA4 RA_MA5 RA_MA6 RA_MA7
    NOLINK
    ITO CHAIN_MODSCAN{N}   Equal El1={X}  El2=CHAIN_MOD_NOLINK_REF  Exit=LD_FLAG
    ITO CHAIN_MODNLADD{N}  Add   El1=LD_CH_NOLINK_MOD El2=LD_FLAG Exit=LD_CH_NOLINK_MOD
    ITO CHAIN_MODANE{N}    Equal El1={X}  El2=CHAIN_MOD_ANON_REF    Exit=LD_FLAG
    ITO CHAIN_MODANADD{N}  Add   El1=LD_CH_ANON_MOD   El2=LD_FLAG Exit=LD_CH_ANON_MOD
/Apply ANON modifier: if set, clear LD_CH_BASE so naming-mode dispatch
/falls into the anonymous branch. LD_CH_OWN_NAME is already preserved above.
JZ CHAIN_ANONCK LD_CH_ANON_MOD CHAIN_RDBODY
CLEAR CHAIN_ANON_CLEAR LD_CH_BASE
RVOCA CHAIN_RDBODY       LOAD_READ_BODY
/Walk body buffer
ITO CHAIN_WALK_INIT      Move  El1=LD_BODY_BUF_BASE_VAL  Exit=LD_NI_PTR
ITO CHAIN_WALK_LOOP      Equal El1=LD_NI_PTR  El2=LD_BODY_PTR  Exit=LD_FLAG
ITO CHAIN_WALK_LCKJ      JumpIf El1=LD_FLAG  Exit=CHAIN_DONE
/Read marker byte
READ_BODY CHAIN_MARK_RD LD_NI_MARK
/marker=C_1 → new lux in chain
JEQ CHAIN_MRK1CK         LD_NI_MARK C_1 CHAIN_NEW_LUX
/marker=C_2 → sub-lux dispatch (RVOCA/RREDI/etc)
RVOCA CHAIN_SUB_COPY     LOAD_COPY_LINE
RVOCA CHAIN_SUB_DISP     LOAD_DISPATCH_BUILT_LINE
ITO CHAIN_SUB_JMP        Jump  Exit=CHAIN_WALK_LOOP
NOLINK
/CHAIN_NEW_LUX: entry point for a new top-level item in the chain.
/Handles: per-item NOLINK emission (when | NOLINK modifier active),
/the '-' link-override marker, then naming-mode dispatch.
ITO CHAIN_NEW_LUX        Move  El1=C_0        Exit=LD_CH_LINKOVR
/Check for the '-' link-override marker: a separate leading token on
/the body line. Only meaningful when the NOLINK modifier is active --
/suppresses this item's per-item NOLINK so it auto-links to whatever
/came right before it. Detected the same way other single-char markers
/are: read the byte directly at the current walk position.
ITO CHAIN_DASHPK   Read  El1=LD_NI_PTR    Exit=LD_TMP
JEQ CHAIN_DASHCK   LD_TMP MINUS CHAIN_DASH_FOUND
ITO CHAIN_DASHSKIP Jump  Exit=CHAIN_NLCK
NOLINK
ITO CHAIN_DASH_FOUND Add   El1=LD_NI_PTR  El2=C_1  Exit=LD_NI_PTR
ITO CHAIN_DASH_ADV2  Add   El1=LD_NI_PTR  El2=C_1  Exit=LD_NI_PTR
ITO CHAIN_DASH_SETOV Move  El1=C_1        Exit=LD_CH_LINKOVR
/Emit per-item NOLINK iff (NOLINK modifier active) and not (overridden).
NOLINK
JZ CHAIN_NLCK      LD_CH_NOLINK_MOD CHAIN_MODE_CK
JZ CHAIN_OVRCK     LD_CH_LINKOVR    CHAIN_DOEMIT
ITO CHAIN_OVRSKIP  Jump  Exit=CHAIN_MODE_CK
NOLINK
RVOCA CHAIN_DOEMIT   NOLINK
/Naming-mode dispatch: 3 cases based on CH_BASE and CH_FULLY_ANON.
/All cases: CHAIN | ANON sets CH_BASE=0 after setup, so it falls into
/the anonymous branch naturally.
NOLINK
JZ CHAIN_MODE_CK LD_CH_BASE CHAIN_ANON
/Check fully anonymous mode (CHAIN _)
JEQ CHAIN_FANON_DISP LD_CH_FULLY_ANON C_1 CHAIN_FULLY_ANON
/Named mode: build auto-name then dispatch
ITO CHAIN_NAMED_JMP   Jump  Exit=CHAIN_DO_BUILD
/CHAIN | ANON: LD_CH_BASE was cleared above, so JZ fires and
/leads here. Route to CHAIN_FULLY_ANON (not manual mode) when
/the ANON modifier is active.
NOLINK
JEQ CHAIN_ANON_MOD_CK LD_CH_ANON_MOD C_1 CHAIN_FULLY_ANON
/Bare CHAIN (manual mode): copy line as "ITO NAME op args" -- body has explicit names
NOLINK
RVOCA CHAIN_ANON         LOAD_COPY_LINE_ITO
RVOCA CHAIN_ANON_DISP    LOAD_DISPATCH_BUILT_LINE
ITO CHAIN_ANON_JMP       Jump  Exit=CHAIN_WALK_LOOP
/Fully anonymous (CHAIN _ or CHAIN NAME | ANON): emit "ITO __ch_N op args"
/Both reach here; CHAIN NAME | ANON additionally emits ALIAS on first item.
NOLINK
RVOCA CHAIN_FULLY_ANON   CHAIN_BUILD_ANON_NAME
RVOCA CHAIN_FA_DISP      LOAD_DISPATCH_BUILT_LINE
ITO CHAIN_FA_IDX         Add   El1=LD_CH_IDX  El2=C_1          Exit=LD_CH_IDX
/CHAIN NAME | ANON: emit "ALIAS OWN_NAME __ch_0" on the first item only
/so the block's own name resolves to the first generated anonymous item.
JZ CHAIN_ALIAS_CK1 LD_CH_OWN_NAME CHAIN_FA_JMP
JEQ CHAIN_ALIAS_CK2 LD_CH_ALIASED C_1 CHAIN_FA_JMP
ITO CHAIN_ALIAS_SET Move El1=C_1 Exit=LD_CH_ALIASED
RVOCA CHAIN_ALIAS_EMIT   CHAIN_BUILD_ALIAS
NOLINK
ITO CHAIN_FA_JMP         Jump  Exit=CHAIN_WALK_LOOP
/Named: build "ITO NAME_N op args" and dispatch
NOLINK
RVOCA CHAIN_DO_BUILD     CHAIN_BUILD_NAME
RVOCA CHAIN_DO_BUILD_D   LOAD_DISPATCH_BUILT_LINE
ITO CHAIN_DO_BUILD_IDX   Add   El1=LD_CH_IDX  El2=C_1          Exit=LD_CH_IDX
ITO CHAIN_DO_BUILD_JMP   Jump  Exit=CHAIN_WALK_LOOP
RREDI CHAIN_DONE
/── CHAIN_BUILD_ALIAS: emit "ALIAS OWN_NAME __ch_{N-1}" ─────────────────────
/Called on the first item of CHAIN NAME | ANON to make the block's own
/name resolve identically to the first generated anonymous item -- so
/NAME is callable/readable as a true alias to __ch_0 (same address).
NEWREF CHAIN_BUILD_ALIAS CHAIN_BA_IMPL
NOLINK
ITO CHAIN_BA_IMPL    Move  El1=BS_TOKBUF_BASE  Exit=LD_NI_OUT
/Write "ALIAS "
WRITE_OUT CHAIN_BA_A  ASCII_A
WRITE_OUT CHAIN_BA_L  ASCII_L
WRITE_OUT CHAIN_BA_I  ASCII_il
WRITE_OUT CHAIN_BA_A2 ASCII_A
WRITE_OUT CHAIN_BA_S  ASCII_S
WRITE_OUT CHAIN_BA_SP SP
/Write OWN_NAME (the block's original name, packed string)
ITO CHAIN_BA_SETBASE Move  El1=LD_CH_OWN_NAME  Exit=LD_CH_BASE
RVOCA CHAIN_BA_ONAME CHAIN_WRITE_PACKED
WRITE_OUT CHAIN_BA_SP2 SP
/Write the generated name: "__ch_" + (LD_CH_IDX-1)
/LD_CH_IDX was already incremented, so current item = LD_CH_IDX-1
WRITE_OUT CHAIN_BA_U1 UNDERSCORE
WRITE_OUT CHAIN_BA_U2 UNDERSCORE
WRITE_OUT CHAIN_BA_C  ASCII_C
WRITE_OUT CHAIN_BA_H  ASCII_H
WRITE_OUT CHAIN_BA_UN UNDERSCORE
ITO CHAIN_BA_PREV    Sub   El1=LD_CH_IDX  El2=C_1  Exit=LD_CH_IDX
RVOCA CHAIN_BA_NUM   CHAIN_WRITE_NUM
ITO CHAIN_BA_RESINC  Add   El1=LD_CH_IDX  El2=C_1  Exit=LD_CH_IDX
ITO CHAIN_BA_TLEN    Sub   El1=LD_NI_OUT  El2=BS_TOKBUF_BASE  Exit=RA_LOAD_TLEN
RVOCA CHAIN_BA_DISP  LOAD_DISPATCH_BUILT_LINE
RREDI CHAIN_BA_RRET
/── LOAD_COPY_LINE: copy current body line bytes to BS_TOKBUF_BASE ───────────
/Reads from LD_NI_PTR until LF or end of buf. Writes to BS_TOKBUF_BASE.
/Advances LD_NI_PTR past the LF.
NEWREF LOAD_COPY_LINE LOAD_CL_IMPL
NEW LD_NI_OUT   /output write ptr into BS_TOKBUF_BASE

/── LOAD_COPY_BODY: shared inner loop — copy LD_NI_PTR→LD_NI_OUT until LF/end ──
/Leaf. Caller sets LD_NI_OUT. Writes NUL at end.
NEWREF LOAD_COPY_BODY LOAD_CPB_LOOP
NOLINK
ITO LOAD_CPB_LOOP        Equal El1=LD_NI_PTR  El2=LD_BODY_PTR  Exit=LD_FLAG
ITO LOAD_CPB_LCKJ        JumpIf El1=LD_FLAG  Exit=LOAD_CPB_DONE
READ_BODY LOAD_CPB_RD LD_TMP
JEQ LOAD_CPB_LFCK        LD_TMP LF LOAD_CPB_DONE
WRITE_OUT LOAD_CPB_WR LD_TMP
ITO LOAD_CPB_JMP         Jump  Exit=LOAD_CPB_LOOP
NOLINK
ITO LOAD_CPB_DONE        Write El1=LD_NI_OUT  El2=C_0
RREDI LOAD_CPB_RET
/── LOAD_COPY_LINE: copy current body line to BS_TOKBUF_BASE ─────────────────
NOLINK
ITO LOAD_CL_IMPL        Move  El1=BS_TOKBUF_BASE  Exit=LD_NI_OUT
RVOCA LOAD_CL_BODY  LOAD_COPY_BODY
ITO LOAD_CL_TLEN        Sub El1=LD_NI_OUT El2=BS_TOKBUF_BASE Exit=RA_LOAD_TLEN
RREDI LOAD_CL_RRET
/── LOAD_COPY_LINE_ITO: like LOAD_COPY_LINE but prepends "ITO " ─────────────
NEWREF LOAD_COPY_LINE_ITO LOAD_CLI_IMPL
NOLINK
ITO LOAD_CLI_IMPL       Move  El1=BS_TOKBUF_BASE  Exit=LD_NI_OUT
/Write "ITO " prefix then copy line body
WRITE_OUT LOAD_CLI_I ASCII_I
WRITE_OUT LOAD_CLI_T ASCII_T
WRITE_OUT LOAD_CLI_O ASCII_O
WRITE_OUT LOAD_CLI_SP SP
RVOCA LOAD_CLI_BODY LOAD_COPY_BODY
ITO LOAD_CLI_TLEN       Sub El1=LD_NI_OUT El2=BS_TOKBUF_BASE Exit=RA_LOAD_TLEN
RREDI LOAD_CLI_RRET
/── CHAIN_BUILD_NAME: build "ITO BASE_SUFFIX_N op args" in tokbuf ─────────────
/Reads current line from body buf (LD_NI_PTR).
/First token of line: if starts with '_' → suffix to add before _N.
/                     else              → op (no suffix, just _N).
/Builds: "ITO BASE_SUFFIX_N op args" or "ITO BASE_N op args".
/Advances LD_NI_PTR past the line.
NEWREF CHAIN_BUILD_NAME CHAIN_BN_IMPL
NEW LD_BN_TOK1_LEN    /length of first token
NEW LD_BN_HAS_SUFFIX  /1 if first token starts with '_'
NOLINK
ITO CHAIN_BN_IMPL        Move  El1=BS_TOKBUF_BASE  Exit=LD_NI_OUT
/Write "ITO " prefix
WRITE_OUT CHAIN_BN_I ASCII_I
WRITE_OUT CHAIN_BN_T ASCII_T
WRITE_OUT CHAIN_BN_O ASCII_O
WRITE_OUT CHAIN_BN_SP SP
/Write base name (LD_CH_BASE packed string: walk and copy bytes until NUL)
RVOCA CHAIN_BN_BASE      CHAIN_WRITE_PACKED
/Peek first byte of line to check for '_'
ITO CHAIN_BN_PEEK        Read  El1=LD_NI_PTR  Exit=LD_TMP
JEQ CHAIN_BN_USCR        LD_TMP UNDERSCORE CHAIN_BN_SUFFIX
/No underscore: no suffix, just write '_' + N
CLEAR CHAIN_BN_NOSUF     LD_BN_HAS_SUFFIX
WRITE_OUT CHAIN_BN_USCR2 UNDERSCORE
RVOCA CHAIN_BN_WNUM2     CHAIN_WRITE_NUM
WRITE_OUT CHAIN_BN_SP2 SP
ITO CHAIN_BN_REST2       Jump  Exit=CHAIN_BN_COPY_REST
/Underscore prefix: skip '_', read suffix until space, then write '_SUFFIX_N'
NOLINK
ITO CHAIN_BN_SUFFIX      Add   El1=LD_NI_PTR  El2=C_1          Exit=LD_NI_PTR
WRITE_OUT CHAIN_BN_USCR_W UNDERSCORE
/Copy suffix chars until space or LF
READ_BODY CHAIN_BN_SFXLOOP LD_TMP
JEQ CHAIN_BN_SFXSP       LD_TMP SP CHAIN_BN_SFXDONE
JEQ CHAIN_BN_SFXLF       LD_TMP LF CHAIN_BN_SFXDONE
WRITE_OUT CHAIN_BN_SFXWR LD_TMP
ITO CHAIN_BN_SFXJMP      Jump  Exit=CHAIN_BN_SFXLOOP
NOLINK
/Write '_N' after suffix
ITO CHAIN_BN_SFXDONE     Write El1=LD_NI_OUT  El2=UNDERSCORE
ITO CHAIN_BN_SFXDINC     Add   El1=LD_NI_OUT  El2=C_1          Exit=LD_NI_OUT
RVOCA CHAIN_BN_WNUM      CHAIN_WRITE_NUM
WRITE_OUT CHAIN_BN_SP3 SP
/Copy rest of line (op + args)
/Copy rest of line (op + args) reusing shared copy body
NOLINK
RVOCA CHAIN_BN_COPY_REST LOAD_COPY_BODY
ITO CHAIN_BN_TLEN        Sub El1=LD_NI_OUT El2=BS_TOKBUF_BASE Exit=RA_LOAD_TLEN
RREDI CHAIN_BN_RRET
/── CHAIN_WRITE_NUM: write decimal digits of LD_CH_IDX to LD_NI_OUT ──────────
/Simple: handles 0-999. Uses LOAD_EIB_IMPL mechanism.
NEWREF CHAIN_WRITE_NUM CHAIN_WN_IMPL
/Write decimal digits of LD_CH_IDX to LD_NI_OUT. Handles 0-999.
NEW LD_WN_VAL  /value being written
NEW LD_WN_DIV  /quotient scratch
NOLINK
ITO CHAIN_WN_IMPL        Move  El1=LD_CH_IDX  Exit=LD_WN_VAL
/Hundreds
ITO CHAIN_WN_H           Div   El1=LD_WN_VAL  El2=C_100       Exit=LD_WN_DIV
ITO CHAIN_WN_HR          Rem   El1=LD_WN_VAL  El2=C_100       Exit=LD_WN_VAL
JZ CHAIN_WN_HCK          LD_WN_DIV CHAIN_WN_TENS
ITO CHAIN_WN_HA          Add   El1=LD_WN_DIV  El2=ASCII_0     Exit=LD_TMP
ITO CHAIN_WN_HW          Write El1=LD_NI_OUT  El2=LD_TMP
ITO CHAIN_WN_HINC        Add   El1=LD_NI_OUT  El2=C_1         Exit=LD_NI_OUT
/Tens
ITO CHAIN_WN_TENS        Div   El1=LD_WN_VAL  El2=C_10        Exit=LD_WN_DIV
ITO CHAIN_WN_TR          Rem   El1=LD_WN_VAL  El2=C_10        Exit=LD_WN_VAL
JZ CHAIN_WN_TCK          LD_WN_DIV CHAIN_WN_ONES
ITO CHAIN_WN_TA          Add   El1=LD_WN_DIV  El2=ASCII_0     Exit=LD_TMP
ITO CHAIN_WN_TW          Write El1=LD_NI_OUT  El2=LD_TMP
ITO CHAIN_WN_TINC        Add   El1=LD_NI_OUT  El2=C_1         Exit=LD_NI_OUT
/Ones (always emit)
ITO CHAIN_WN_ONES        Add   El1=LD_WN_VAL  El2=ASCII_0     Exit=LD_TMP
ITO CHAIN_WN_OW          Write El1=LD_NI_OUT  El2=LD_TMP
ITO CHAIN_WN_OINC        Add   El1=LD_NI_OUT  El2=C_1         Exit=LD_NI_OUT
RREDI CHAIN_WN_RRET
/── CHAIN_BUILD_ANON_NAME: build "ITO __ch_N op args" in tokbuf ───────────────
/Writes "ITO __ch_N " prefix then copies rest of current body line.
NEWREF CHAIN_BUILD_ANON_NAME CHAIN_BAN_IMPL
NOLINK
ITO CHAIN_BAN_IMPL       Move  El1=BS_TOKBUF_BASE  Exit=LD_NI_OUT
/Write "ITO __ch_"
WRITE_OUT CHAIN_BAN_I ASCII_I
WRITE_OUT CHAIN_BAN_T ASCII_T
WRITE_OUT CHAIN_BAN_O ASCII_O
WRITE_OUT CHAIN_BAN_SP SP
/Write "__ch_"
WRITE_OUT CHAIN_BAN_U1 UNDERSCORE
WRITE_OUT CHAIN_BAN_U2 UNDERSCORE
WRITE_OUT CHAIN_BAN_C ASCII_cl
WRITE_OUT CHAIN_BAN_H ASCII_hl
WRITE_OUT CHAIN_BAN_U3 UNDERSCORE
/Write N (decimal index)
RVOCA CHAIN_BAN_NUM      CHAIN_WRITE_NUM
/Write space + copy rest of line
WRITE_OUT CHAIN_BAN_SP2 SP
RVOCA CHAIN_BAN_REST     LOAD_COPY_BODY
ITO CHAIN_BAN_TLEN       Sub El1=LD_NI_OUT El2=BS_TOKBUF_BASE Exit=RA_LOAD_TLEN
RREDI CHAIN_BAN_RRET
/── CHAIN_WRITE_PACKED: write packed string bytes of LD_CH_BASE to LD_NI_OUT ──
/Walks lux chain of LD_CH_BASE, extracts bytes (little-endian 8 per lux),
/writes until NUL byte.
NEWREF CHAIN_WRITE_PACKED CHAIN_WP_IMPL
NEW LD_WP_LUX    /current lux in packed string
NEW LD_WP_WORD   /current 8-byte word
NEW LD_WP_SHIFT  /bit shift (0,8,16,24,32,40,48,56)
NOLINK
ITO CHAIN_WP_IMPL        Move  El1=LD_CH_BASE  Exit=LD_WP_LUX
/Read word from lux
ITO CHAIN_WP_RDLUX       Read  El1=LD_WP_LUX  Exit=LD_WP_WORD
CLEAR CHAIN_WP_SHFT      LD_WP_SHIFT
/Extract bytes one at a time
ITO CHAIN_WP_BYTELOOP    Right El1=LD_WP_WORD  El2=LD_WP_SHIFT  Exit=LD_TMP
ITO CHAIN_WP_MASK        And   El1=LD_TMP      El2=C_255          Exit=LD_TMP
/NUL = end
JZ CHAIN_WP_NULCK        LD_TMP CHAIN_WP_DONE
/Write byte
WRITE_OUT CHAIN_WP_WR LD_TMP
/Advance shift; if shift reaches 64, move to next lux
ITO CHAIN_WP_SHINC       Add   El1=LD_WP_SHIFT El2=C_8           Exit=LD_WP_SHIFT
JEQ CHAIN_WP_SHCK        LD_WP_SHIFT C_64 CHAIN_WP_NEXTLUX
ITO CHAIN_WP_SHJMP       Jump  Exit=CHAIN_WP_BYTELOOP
NOLINK
/Move to next lux in chain (slot 5 = Next)
ITO CHAIN_WP_NEXTLUX     Add   El1=LD_WP_LUX  El2=SLOT_NEXT           Exit=LD_TMP
ITO CHAIN_WP_NXTRD       Read  El1=LD_TMP      Exit=LD_WP_LUX
JZ CHAIN_WP_NXTCK        LD_WP_LUX CHAIN_WP_DONE
ITO CHAIN_WP_NXTJMP      Jump  Exit=CHAIN_WP_RDLUX
NOLINK
RREDI CHAIN_WP_DONE