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
NEWREF WIRE_AUTOLINK WA_W0
NOLINK
ITO WA_W0   Move  El1=RA_MC_LUX   Exit=RA_MC_SLOT
ITO WA_WW   Write El1=RA_MC_SLOT   El2=RA_MC_LUX
ITO WA_A1   Add   El1=RA_MC_LUX   El2=C_1         Exit=RA_MC_SLOT
ITO WA_W1   Write El1=RA_MC_SLOT   El2=RA_MC_OP
ITO WA_A2   Add   El1=RA_MC_LUX   El2=C_2         Exit=RA_MC_SLOT
ITO WA_W2   Write El1=RA_MC_SLOT   El2=RA_MC_E1
ITO WA_A3   Add   El1=RA_MC_LUX   El2=C_3         Exit=RA_MC_SLOT
ITO WA_W3   Write El1=RA_MC_SLOT   El2=RA_MC_E2
ITO WA_A4   Add   El1=RA_MC_LUX   El2=C_4         Exit=RA_MC_SLOT
ITO WA_W4   Write El1=RA_MC_SLOT   El2=RA_MC_DEST
ITO WA_SRL  Move  El1=RA_LINK      Exit=RA_MC_TMP_RL
ITO WA_CAL  Voca  El1=AUTOLINK     Exit=RA_LINK
NOLINK
ITO WA_RST  Move  El1=RA_MC_TMP_RL Exit=RA_LINK
RREDI WA_RET
/── WIRE_AUTOLINK_RESET: like WIRE_AUTOLINK but uses AUTOLINK_RESET (chain terminator)
NEWREF WIRE_AUTOLINK_RESET WAR_W0
NOLINK
ITO WAR_W0   Move  El1=RA_MC_LUX   Exit=RA_MC_SLOT
ITO WAR_WW   Write El1=RA_MC_SLOT   El2=RA_MC_LUX
ITO WAR_A1   Add   El1=RA_MC_LUX   El2=C_1         Exit=RA_MC_SLOT
ITO WAR_W1   Write El1=RA_MC_SLOT   El2=RA_MC_OP
ITO WAR_A2   Add   El1=RA_MC_LUX   El2=C_2         Exit=RA_MC_SLOT
ITO WAR_W2   Write El1=RA_MC_SLOT   El2=RA_MC_E1
ITO WAR_A3   Add   El1=RA_MC_LUX   El2=C_3         Exit=RA_MC_SLOT
ITO WAR_W3   Write El1=RA_MC_SLOT   El2=RA_MC_E2
ITO WAR_A4   Add   El1=RA_MC_LUX   El2=C_4         Exit=RA_MC_SLOT
ITO WAR_W4   Write El1=RA_MC_SLOT   El2=RA_MC_DEST
ITO WAR_SRL  Move  El1=RA_LINK      Exit=RA_MC_TMP_RL
ITO WAR_CAL  Voca  El1=AUTOLINK_RESET Exit=RA_LINK
NOLINK
ITO WAR_RST  Move  El1=RA_MC_TMP_RL Exit=RA_LINK
RREDI WAR_RET
/── RVOCA: RVOCA name sub → ITO name Voca El1=sub Exit=RA_LINK
/MA0=name_addr  MA1=sub_addr
NEWREF RVOCA RVOCA_SPN RVOCA name sub → ITO name Voca El1=sub Exit=RA_LINK
NOLINK
ITO RVOCA_SPN  Move  El1=RA_MA0       Exit=RA_MC_LUX
ITO RVOCA_OP   Move  El1=Voca         Exit=RA_MC_OP
ITO RVOCA_E1   Move  El1=RA_MA1       Exit=RA_MC_E1
ITO RVOCA_E2   Move  El1=C_0          Exit=RA_MC_E2
ITO RVOCA_DST  Move  El1=RA_LINK_REF  Exit=RA_MC_DEST
ITO RVOCA_SRL  Move  El1=RA_LINK      Exit=RA_MC_TMP_RL
ITO RVOCA_CAL  Voca  El1=WIRE_AUTOLINK Exit=RA_LINK
NOLINK
ITO RVOCA_RST  Move  El1=RA_MC_TMP_RL Exit=RA_LINK
RREDI RVOCA_RET
/── RREDI: RREDI name → ITO name Redi El1=RA_LINK + reset _last_ito
/MA0=name_addr
NEWREF RREDI RREDI_SPN RREDI name → ITO name Redi El1=RA_LINK + reset _last_ito
NOLINK
ITO RREDI_SPN  Move  El1=RA_MA0       Exit=RA_MC_LUX
ITO RREDI_OP   Move  El1=Redi         Exit=RA_MC_OP
ITO RREDI_E1   Move  El1=RA_LINK_REF  Exit=RA_MC_E1
ITO RREDI_E2   Move  El1=C_0          Exit=RA_MC_E2
ITO RREDI_DST  Move  El1=C_0          Exit=RA_MC_DEST
ITO RREDI_SRL  Move  El1=RA_LINK      Exit=RA_MC_TMP_RL
ITO RREDI_CAL  Voca  El1=WIRE_AUTOLINK_RESET Exit=RA_LINK
NOLINK
ITO RREDI_RST  Move  El1=RA_MC_TMP_RL Exit=RA_LINK
RREDI RREDI_RET
/── CLEAR: CLEAR name target → ITO name Move El1=C_0 Exit=target
/MA0=name_addr  MA1=target_addr
NEWREF CLEAR CLEAR_SPN CLEAR name target → ITO name Move El1=C_0 Exit=target
NOLINK
ITO CLEAR_SPN  Move  El1=RA_MA0       Exit=RA_MC_LUX
ITO CLEAR_OP   Move  El1=Move         Exit=RA_MC_OP
ITO CLEAR_E1   Move  El1=RA_C0_REF    Exit=RA_MC_E1
ITO CLEAR_E2   Move  El1=C_0          Exit=RA_MC_E2
ITO CLEAR_DST  Move  El1=RA_MA1       Exit=RA_MC_DEST
ITO CLEAR_SRL  Move  El1=RA_LINK      Exit=RA_MC_TMP_RL
ITO CLEAR_CAL  Voca  El1=WIRE_AUTOLINK Exit=RA_LINK
NOLINK
ITO CLEAR_RST  Move  El1=RA_MC_TMP_RL Exit=RA_LINK
RREDI CLEAR_RET
/── NOP: NOP name → ITO name Move El1=C_0 Exit=C_0
/MA0=name_addr
NEWREF NOP NOP_SPN NOP name → ITO name Move El1=C_0 Exit=C_0
NOLINK
ITO NOP_SPN    Move  El1=RA_MA0       Exit=RA_MC_LUX
ITO NOP_OP     Move  El1=Move         Exit=RA_MC_OP
ITO NOP_E1     Move  El1=RA_C0_REF    Exit=RA_MC_E1
ITO NOP_E2     Move  El1=C_0          Exit=RA_MC_E2
ITO NOP_DST    Move  El1=RA_C0_REF    Exit=RA_MC_DEST
ITO NOP_SRL    Move  El1=RA_LINK      Exit=RA_MC_TMP_RL
ITO NOP_CAL    Voca  El1=WIRE_AUTOLINK Exit=RA_LINK
NOLINK
ITO NOP_RST    Move  El1=RA_MC_TMP_RL Exit=RA_LINK
RREDI NOP_RET
/── ALLOC_TO: ALLOC_TO name dest count → Move count→RA_ALLOC_COUNT; ALLOC_LUCES; Move result→dest
/MA0=name MA1=dest MA2=count MA3=name_J(auto _J) MA4=name_K(auto _K)
NEWREF ALLOC_TO ALLOC_TO_SPN
/── ALLOC_TO: ALLOC_TO name dest size ─────────────────────────────────────
/Generates 3 ITOs: name(Move size→RA_ALLOC_COUNT), name_J(Voca ALLOC_LUCES),
/name_K(Move RA_ALLOC_RESULT→dest).
/
/USE when: you need the result in a specific register OTHER than RA_ALLOC_RESULT.
/  e.g. ALLOC_TO BS_DI_CNT RA_BS_EL1 ITO_SIZE   ← writes result into RA_BS_EL1
/
/DO NOT USE when: dest == RA_ALLOC_RESULT. The name_K ITO becomes a NOP and
/adds an unwired (op=0) builder-class lux. Use plain Move+RVOCA instead:
/  ITO name  Move El1=size Exit=RA_ALLOC_COUNT
/  RVOCA name_J ALLOC_LUCES
NOLINK
/Wire name: Move size → RA_ALLOC_COUNT
ITO ALLOC_TO_SPN  Move  El1=RA_MA0         Exit=RA_MC_LUX
ITO ALLOC_TO_OP   Move  El1=Move           Exit=RA_MC_OP
ITO ALLOC_TO_E1   Move  El1=RA_MA2         Exit=RA_MC_E1
CLEAR ALLOC_TO_E2 RA_MC_E2
ITO ALLOC_TO_DST  Move  El1=RA_ALLOC_COUNT Exit=RA_MC_DEST
RVOCA ALLOC_TO_W1 WRITE_ITO_SLOTS
/Link name→name_J; Wire name_J: Voca ALLOC_LUCES
ITO ALLOC_TO_NLK  Add   El1=RA_MA0  El2=C_5 Exit=RA_MC_SLOT
ITO ALLOC_TO_NLKW Write El1=RA_MC_SLOT El2=RA_MA3
ITO ALLOC_TO_J0   Move  El1=RA_MA3         Exit=RA_MC_LUX
ITO ALLOC_TO_JOP  Move  El1=Voca           Exit=RA_MC_OP
ITO ALLOC_TO_JE1  Move  El1=ALLOC_LUCES    Exit=RA_MC_E1
CLEAR ALLOC_TO_JE2 RA_MC_E2
ITO ALLOC_TO_JDST Move  El1=RA_LINK_REF    Exit=RA_MC_DEST
RVOCA ALLOC_TO_W2 WRITE_ITO_SLOTS
/Link name_J→name_K; Wire name_K: Move RA_ALLOC_RESULT → dest
ITO ALLOC_TO_JNLK Add   El1=RA_MA3  El2=C_5 Exit=RA_MC_SLOT
ITO ALLOC_TO_JNLKW Write El1=RA_MC_SLOT El2=RA_MA4
ITO ALLOC_TO_K0   Move  El1=RA_MA4         Exit=RA_MC_LUX
ITO ALLOC_TO_KOP  Move  El1=Move           Exit=RA_MC_OP
ITO ALLOC_TO_KE1  Move  El1=RA_ALLOC_RESULT Exit=RA_MC_E1
CLEAR ALLOC_TO_KE2 RA_MC_E2
ITO ALLOC_TO_KDST Move  El1=RA_MA1         Exit=RA_MC_DEST
RVOCA ALLOC_TO_W3 WRITE_ITO_SLOTS
RREDI ALLOC_TO_RET
NEW RA_MC_FLAG   /scratch: flag data lux addr (for JEQ/JZ)
SETREF RA_MC_FLAG RA_MC_FLAG  /self-ref
NEW RA_MC_OP     /scratch: op addr
NEW RA_MC_E1      /scratch: arg A
NEW RA_MC_E2      /scratch: arg B
NEW RA_MC_DEST   /scratch: destination addr
NEW RA_MC_J      /scratch: second lux (_J suffix)
NEW RA_MC_K      /scratch: third lux (_K suffix)
NEW RA_MC_TMP_RL /scratch: saved RA_LINK for AUTOLINK call
NEW RA_MC_WIS_RL /scratch: saved RA_LINK inside WRITE_ITO_SLOTS

/── WRITE_ITO_SLOTS: write all 5 ITO slots then wire autolink ──
/IN: RA_MC_LUX=addr, RA_MC_OP=op, RA_MC_E1=e1, RA_MC_E2=e2, RA_MC_DEST=exit
/Writes: word(self), op, e1, e2, exit. Then calls WIRE_AUTOLINK.
/Uses RA_MC_WIS_RL as own scratch — safe to call from JEQ/JZ/LINK etc.
NOLINK
ITO WIS_W0    Move  El1=RA_MC_LUX  Exit=RA_MC_SLOT
ITO WIS_WW    Write El1=RA_MC_SLOT El2=RA_MC_LUX
ITO WIS_A1    Add   El1=RA_MC_LUX  El2=C_1        Exit=RA_MC_SLOT
ITO WIS_OP    Write El1=RA_MC_SLOT El2=RA_MC_OP
ITO WIS_A2    Add   El1=RA_MC_LUX  El2=C_2        Exit=RA_MC_SLOT
ITO WIS_E1    Write El1=RA_MC_SLOT El2=RA_MC_E1
ITO WIS_A3    Add   El1=RA_MC_LUX  El2=C_3        Exit=RA_MC_SLOT
ITO WIS_E2    Write El1=RA_MC_SLOT El2=RA_MC_E2
ITO WIS_A4    Add   El1=RA_MC_LUX  El2=C_4        Exit=RA_MC_SLOT
ITO WIS_EX    Write El1=RA_MC_SLOT El2=RA_MC_DEST
ITO WIS_SAV   Move  El1=RA_LINK    Exit=RA_MC_WIS_RL
ITO WIS_CAL   Voca  El1=WIRE_AUTOLINK Exit=RA_LINK
NOLINK
ITO WIS_RST   Move  El1=RA_MC_WIS_RL Exit=RA_LINK
RREDI WIS_RET
NEWREF WRITE_ITO_SLOTS WIS_W0

/── WRITE_ITO_SLOTS_RESET: like WRITE_ITO_SLOTS but calls WIRE_AUTOLINK_RESET ──
/Used by chain terminators (RCALL_AT name_J, RREDI) — resets RA_MC_PREV after linking.
NOLINK
ITO WISR_W0   Move  El1=RA_MC_LUX  Exit=RA_MC_SLOT
ITO WISR_WW   Write El1=RA_MC_SLOT El2=RA_MC_LUX
ITO WISR_A1   Add   El1=RA_MC_LUX  El2=C_1        Exit=RA_MC_SLOT
ITO WISR_OP   Write El1=RA_MC_SLOT El2=RA_MC_OP
ITO WISR_A2   Add   El1=RA_MC_LUX  El2=C_2        Exit=RA_MC_SLOT
ITO WISR_E1   Write El1=RA_MC_SLOT El2=RA_MC_E1
ITO WISR_A3   Add   El1=RA_MC_LUX  El2=C_3        Exit=RA_MC_SLOT
ITO WISR_E2   Write El1=RA_MC_SLOT El2=RA_MC_E2
ITO WISR_A4   Add   El1=RA_MC_LUX  El2=C_4        Exit=RA_MC_SLOT
ITO WISR_EX   Write El1=RA_MC_SLOT El2=RA_MC_DEST
ITO WISR_SAV  Move  El1=RA_LINK    Exit=RA_MC_WIS_RL
ITO WISR_CAL  Voca  El1=WIRE_AUTOLINK_RESET Exit=RA_LINK
NOLINK
ITO WISR_RST  Move  El1=RA_MC_WIS_RL Exit=RA_LINK
ITO WISR_RET  Redi El1=RA_LINK
/NOTE: raw ITO Redi — implementation of reset mechanism, not user of it.

NEWREF WRITE_ITO_SLOTS_RESET WISR_W0

NEWREF AUTOLINK AUTOLINK_JCK
/── AUTOLINK: sub — if RA_MC_PREV!=0 link prev→lux (SLOT_NEXT); set _last_ito=lux
NOLINK
ITO AUTOLINK_JCK  JumpIf El1=RA_MC_PREV   Exit=AUTOLINK_DO
ITO AUTOLINK_SKP  Jump   Exit=AUTOLINK_SET
NOLINK
ITO AUTOLINK_DO   Add    El1=RA_MC_PREV   El2=C_5         Exit=RA_MC_SLOT
ITO AUTOLINK_DW   Write  El1=RA_MC_SLOT   El2=RA_MC_LUX
ITO AUTOLINK_SET  Move   El1=RA_MC_LUX   Exit=RA_MC_PREV
/NOTE: raw ITO Redi — this is the implementation of the autolink mechanism.
/All users of this mechanism use RREDI. The mechanism itself cannot.
ITO AUTOLINK_RET Redi El1=RA_LINK
NEWREF AUTOLINK_RESET AUTORST_JCK
/── AUTOLINK_RESET: like AUTOLINK but resets RA_MC_PREV to 0 after linking.
/Used by RREDI (chain terminator): links prev→lux, then clears prev so the
/next ITO is not auto-linked to the terminator.
NOLINK
ITO AUTORST_JCK  JumpIf El1=RA_MC_PREV   Exit=AUTORST_DO
ITO AUTORST_SKP  Jump   Exit=AUTORST_RST
NOLINK
ITO AUTORST_DO   Add    El1=RA_MC_PREV   El2=C_5         Exit=RA_MC_SLOT
ITO AUTORST_DW   Write  El1=RA_MC_SLOT   El2=RA_MC_LUX
ITO AUTORST_RST  Move   El1=C_0          Exit=RA_MC_PREV
/NOTE: raw ITO Redi — same reason as AUTOLINK_RET above. Implementation, not user.
ITO AUTORST_RET Redi El1=RA_LINK
/── JEQ: JEQ name a b dest → Equal(a,b)→RA_JEQ_FLAG; JumpIf→dest; NOP
/  MA0=name  MA1=a  MA2=b  MA3=dest  MA4=name_J(auto _J)  MA5=name_K(auto _K)
NOLINK
NEWREF JEQ JEQ_N1_SET
/Wire ITO name: Equal(a, b) → RA_JEQ_FLAG
ITO JEQ_N1_SET  Move El1=RA_MA0          Exit=RA_MC_LUX
ITO JEQ_N1_SOP  Move El1=Equal           Exit=RA_MC_OP
ITO JEQ_N1_SE1  Move El1=RA_MA1          Exit=RA_MC_E1
ITO JEQ_N1_SE2  Move El1=RA_MA2          Exit=RA_MC_E2
ITO JEQ_N1_SEX  Move El1=RA_JEQ_FLAG_PTR Exit=RA_MC_DEST
RVOCA JEQ_N1_WIS WRITE_ITO_SLOTS
/Link Equal.nxt → _J
ITO JEQ_N1_NXT  Add  El1=RA_MA0 El2=C_5  Exit=RA_MC_SLOT
ITO JEQ_N1_NW   Write El1=RA_MC_SLOT El2=RA_MA4
/Wire ITO name_J: JumpIf(RA_JEQ_FLAG) → dest
ITO JEQ_N2_SET  Move El1=RA_MA4          Exit=RA_MC_LUX
ITO JEQ_N2_SOP  Move El1=JumpIf          Exit=RA_MC_OP
ITO JEQ_N2_SE1  Move El1=RA_JEQ_FLAG_PTR Exit=RA_MC_E1
CLEAR JEQ_N2_SE2 RA_MC_E2
ITO JEQ_N2_SEX  Move El1=RA_MA3          Exit=RA_MC_DEST
RVOCA JEQ_N2_WIS WRITE_ITO_SLOTS
/Link _J.nxt → _K
ITO JEQ_N2_NXT  Add  El1=RA_MA4 El2=C_5  Exit=RA_MC_SLOT
ITO JEQ_N2_NW   Write El1=RA_MC_SLOT El2=RA_MA5
/Wire ITO name_K: Move C_0 → C_0 (NOP placeholder)
ITO JEQ_N3_SET  Move El1=RA_MA5          Exit=RA_MC_LUX
ITO JEQ_N3_SOP  Move El1=Move            Exit=RA_MC_OP
ITO JEQ_N3_SE1  Move El1=RA_C0_REF       Exit=RA_MC_E1
ITO JEQ_N3_SE2  Move El1=RA_C0_REF       Exit=RA_MC_E2
ITO JEQ_N3_SEX  Move El1=RA_C0_REF       Exit=RA_MC_DEST
RVOCA JEQ_N3_WIS WRITE_ITO_SLOTS
RREDI JEQ_RET
/── JZ: JZ name cond dest → Equal(cond,C_0)→flag; JumpIf→dest; NOP
/  MA0=name MA1=cond MA2=dest MA3=name_J(auto _J) MA4=name_K(auto _K)
NOLINK
NEWREF JZ JZ_N1_SET
/Wire ITO name: Equal(cond, C_0) → RA_JEQ_FLAG
ITO JZ_N1_SET  Move El1=RA_MA0          Exit=RA_MC_LUX
ITO JZ_N1_SOP  Move El1=Equal           Exit=RA_MC_OP
ITO JZ_N1_SE1  Move El1=RA_MA1          Exit=RA_MC_E1
ITO JZ_N1_SE2  Move El1=RA_C0_REF       Exit=RA_MC_E2
ITO JZ_N1_SEX  Move El1=RA_JEQ_FLAG_PTR Exit=RA_MC_DEST
RVOCA JZ_N1_WIS WRITE_ITO_SLOTS
/Link Equal.nxt → _J
ITO JZ_N1_NXT  Add  El1=RA_MA0 El2=C_5  Exit=RA_MC_SLOT
ITO JZ_N1_NW   Write El1=RA_MC_SLOT El2=RA_MA3
/Wire ITO name_J: JumpIf(RA_JEQ_FLAG) → dest
ITO JZ_N2_SET  Move El1=RA_MA3          Exit=RA_MC_LUX
ITO JZ_N2_SOP  Move El1=JumpIf          Exit=RA_MC_OP
ITO JZ_N2_SE1  Move El1=RA_JEQ_FLAG_PTR Exit=RA_MC_E1
CLEAR JZ_N2_SE2 RA_MC_E2
ITO JZ_N2_SEX  Move El1=RA_MA2          Exit=RA_MC_DEST
RVOCA JZ_N2_WIS WRITE_ITO_SLOTS
/Link _J.nxt → _K
ITO JZ_N2_NXT  Add  El1=RA_MA3 El2=C_5  Exit=RA_MC_SLOT
ITO JZ_N2_NW   Write El1=RA_MC_SLOT El2=RA_MA4
/Wire ITO name_K: Move C_0 → C_0 (NOP placeholder)
ITO JZ_N3_SET  Move El1=RA_MA4          Exit=RA_MC_LUX
ITO JZ_N3_SOP  Move El1=Move            Exit=RA_MC_OP
ITO JZ_N3_SE1  Move El1=RA_C0_REF       Exit=RA_MC_E1
ITO JZ_N3_SE2  Move El1=RA_C0_REF       Exit=RA_MC_E2
ITO JZ_N3_SEX  Move El1=RA_C0_REF       Exit=RA_MC_DEST
RVOCA JZ_N3_WIS WRITE_ITO_SLOTS
RREDI JZ_RET
/── LX: LX name src exit_lux → Move src→RA_SR_LUMEN; Voca SR_GLX; Move RA_SR_LUMEN→exit_lux
/  MA0=name  MA1=src  MA2=exit_lux  MA3=name_J(auto _J)  MA4=name_W(auto _W)
NOLINK
NEWREF LX LX_N1_SET
/Wire ITO name: Move src → RA_SR_LUMEN
ITO LX_N1_SET  Move El1=RA_MA0      Exit=RA_MC_LUX
ITO LX_N1_SOP  Move El1=Move        Exit=RA_MC_OP
ITO LX_N1_SE1  Move El1=RA_MA1      Exit=RA_MC_E1
CLEAR LX_N1_SE2 RA_MC_E2
ITO LX_N1_SEX  Move El1=RA_SR_LUMEN Exit=RA_MC_DEST
RVOCA LX_N1_WIS WRITE_ITO_SLOTS
/Wire ITO name_J: Voca SR_GLX Exit=RA_LINK
ITO LX_N2_SET  Move El1=RA_MA3      Exit=RA_MC_LUX
ITO LX_N2_SOP  Move El1=Voca        Exit=RA_MC_OP
ITO LX_N2_SE1  Move El1=SR_GLX      Exit=RA_MC_E1
CLEAR LX_N2_SE2 RA_MC_E2
ITO LX_N2_SEX  Move El1=RA_LINK_REF Exit=RA_MC_DEST
RVOCA LX_N2_WIS WRITE_ITO_SLOTS
/Wire ITO name_W: Move RA_SR_LUMEN → exit_lux
ITO LX_N3_SET  Move El1=RA_MA4      Exit=RA_MC_LUX
ITO LX_N3_SOP  Move El1=Move        Exit=RA_MC_OP
ITO LX_N3_SE1  Move El1=RA_SR_LUMEN Exit=RA_MC_E1
CLEAR LX_N3_SE2 RA_MC_E2
ITO LX_N3_SEX  Move El1=RA_MA2      Exit=RA_MC_DEST
RVOCA LX_N3_WIS WRITE_ITO_SLOTS
RREDI LX_RET
/── LH: LH name src exit_lux → Move C_1→RA_SR_OFFSET; Move src→RA_SR_LUX; Voca SR_GLL; Move RA_SR_LUMEN→exit_lux
/  MA0=name_OFF(auto_OFF)  MA1=name(MA0 arg1)... wait — LH has 4 luces: _OFF,name,_J,_W
/  auto-suffix: _J, _K, _W, _R, _T, _LUX, _REL, _OFF, _OUTER...
/  parts for LH: LH name src exit_lux → MA0=name, MA1=src, MA2=exit_lux, then auto: _J→MA3, _W→MA4, _OFF→MA5
/  But _OFF comes before name in execution order. We use MA5=name_OFF.
NOLINK
NEWREF LH LH_N0_SET
/Wire ITO name_OFF: Move C_1 → RA_SR_OFFSET
ITO LH_N0_SET  Move El1=RA_MA5      Exit=RA_MC_LUX
ITO LH_N0_SOP  Move El1=Move        Exit=RA_MC_OP
ITO LH_N0_SE1  Move El1=C_1         Exit=RA_MC_E1
CLEAR LH_N0_SE2 RA_MC_E2
ITO LH_N0_SEX  Move El1=RA_SR_OFFSET Exit=RA_MC_DEST
RVOCA LH_N0_WIS WRITE_ITO_SLOTS
/Wire ITO name: Move src → RA_SR_LUX
ITO LH_N1_SET  Move El1=RA_MA0      Exit=RA_MC_LUX
ITO LH_N1_SOP  Move El1=Move        Exit=RA_MC_OP
ITO LH_N1_SE1  Move El1=RA_MA1      Exit=RA_MC_E1
CLEAR LH_N1_SE2 RA_MC_E2
ITO LH_N1_SEX  Move El1=RA_SR_LUX   Exit=RA_MC_DEST
RVOCA LH_N1_WIS WRITE_ITO_SLOTS
/Wire ITO name_J: Voca SR_GLL Exit=RA_LINK
ITO LH_N2_SET  Move El1=RA_MA3      Exit=RA_MC_LUX
ITO LH_N2_SOP  Move El1=Voca        Exit=RA_MC_OP
ITO LH_N2_SE1  Move El1=SR_GLL      Exit=RA_MC_E1
CLEAR LH_N2_SE2 RA_MC_E2
ITO LH_N2_SEX  Move El1=RA_LINK_REF Exit=RA_MC_DEST
RVOCA LH_N2_WIS WRITE_ITO_SLOTS
/Wire ITO name_W: Move RA_SR_LUMEN → exit_lux
ITO LH_N3_SET  Move El1=RA_MA4      Exit=RA_MC_LUX
ITO LH_N3_SOP  Move El1=Move        Exit=RA_MC_OP
ITO LH_N3_SE1  Move El1=RA_SR_LUMEN Exit=RA_MC_E1
CLEAR LH_N3_SE2 RA_MC_E2
ITO LH_N3_SEX  Move El1=RA_MA2      Exit=RA_MC_DEST
RVOCA LH_N3_WIS WRITE_ITO_SLOTS
RREDI LH_RET
/── WALK_ONE: WALK_ONE name lux rel → Move lux→RA_SR_LUX; Move rel→RA_SR_REL; Move C_1→RA_SR_OFFSET; Voca SR_WALK_ONE
/  MA0=name  MA1=lux  MA2=rel  MA3=name_LUX(auto)  MA4=name_REL(auto)  MA5=name_OFF(auto)
NOLINK
NEWREF WALK_ONE WALK_N1_SET
/Wire ITO name_LUX: Move lux → RA_SR_LUX
ITO WALK_N1_SET  Move El1=RA_MA3       Exit=RA_MC_LUX
ITO WALK_N1_SOP  Move El1=Move         Exit=RA_MC_OP
ITO WALK_N1_SE1  Move El1=RA_MA1       Exit=RA_MC_E1
CLEAR WALK_N1_SE2 RA_MC_E2
ITO WALK_N1_SEX  Move El1=RA_SR_LUX    Exit=RA_MC_DEST
RVOCA WALK_N1_WIS WRITE_ITO_SLOTS
/Wire ITO name_REL: Move rel → RA_SR_REL
ITO WALK_N2_SET  Move El1=RA_MA4       Exit=RA_MC_LUX
ITO WALK_N2_SOP  Move El1=Move         Exit=RA_MC_OP
ITO WALK_N2_SE1  Move El1=RA_MA2       Exit=RA_MC_E1
CLEAR WALK_N2_SE2 RA_MC_E2
ITO WALK_N2_SEX  Move El1=RA_SR_REL    Exit=RA_MC_DEST
RVOCA WALK_N2_WIS WRITE_ITO_SLOTS
/Wire ITO name_OFF: Move C_1 → RA_SR_OFFSET
ITO WALK_N3_SET  Move El1=RA_MA5       Exit=RA_MC_LUX
ITO WALK_N3_SOP  Move El1=Move         Exit=RA_MC_OP
ITO WALK_N3_SE1  Move El1=C_1          Exit=RA_MC_E1
CLEAR WALK_N3_SE2 RA_MC_E2
ITO WALK_N3_SEX  Move El1=RA_SR_OFFSET Exit=RA_MC_DEST
RVOCA WALK_N3_WIS WRITE_ITO_SLOTS
/Wire ITO name: Voca SR_WALK_ONE Exit=RA_LINK
ITO WALK_N4_SET  Move El1=RA_MA0       Exit=RA_MC_LUX
ITO WALK_N4_SOP  Move El1=Voca         Exit=RA_MC_OP
ITO WALK_N4_SE1  Move El1=SR_WALK_ONE  Exit=RA_MC_E1
CLEAR WALK_N4_SE2 RA_MC_E2
ITO WALK_N4_SEX  Move El1=RA_LINK_REF  Exit=RA_MC_DEST
RVOCA WALK_N4_WIS WRITE_ITO_SLOTS
RREDI WALK_RET
/── LINK_OP: LINK_OP name src rel exit → Move src→RA_LM_SRC; Move rel→RA_LM_REL; Move exit_lux→RA_LM_EXIT; Voca ADD_LUMEN
/  MA0=name  MA1=src  MA2=rel  MA3=exit_lux  MA4=name_R(auto _R)  MA5=name_T(auto _T)  MA6=name_J(auto _J)
NOLINK
NEWREF LINK_OP LINK_N1_SET
/Wire ITO name: Move src → RA_LM_SRC
ITO LINK_N1_SET  Move El1=RA_MA0       Exit=RA_MC_LUX
ITO LINK_N1_SOP  Move El1=Move         Exit=RA_MC_OP
ITO LINK_N1_SE1  Move El1=RA_MA1       Exit=RA_MC_E1
CLEAR LINK_N1_SE2 RA_MC_E2
ITO LINK_N1_SEX  Move El1=RA_LM_SRC    Exit=RA_MC_DEST
RVOCA LINK_N1_WIS WRITE_ITO_SLOTS
/Wire ITO name_R: Move rel → RA_LM_REL
ITO LINK_N2_SET  Move El1=RA_MA4       Exit=RA_MC_LUX
ITO LINK_N2_SOP  Move El1=Move         Exit=RA_MC_OP
ITO LINK_N2_SE1  Move El1=RA_MA2       Exit=RA_MC_E1
CLEAR LINK_N2_SE2 RA_MC_E2
ITO LINK_N2_SEX  Move El1=RA_LM_REL    Exit=RA_MC_DEST
RVOCA LINK_N2_WIS WRITE_ITO_SLOTS
/Wire ITO name_T: Move exit → RA_LM_EXIT
ITO LINK_N3_SET  Move El1=RA_MA5       Exit=RA_MC_LUX
ITO LINK_N3_SOP  Move El1=Move         Exit=RA_MC_OP
ITO LINK_N3_SE1  Move El1=RA_MA3       Exit=RA_MC_E1
CLEAR LINK_N3_SE2 RA_MC_E2
ITO LINK_N3_SEX  Move El1=RA_LM_EXIT   Exit=RA_MC_DEST
RVOCA LINK_N3_WIS WRITE_ITO_SLOTS
/Wire ITO name_J: Voca ADD_LUMEN Exit=RA_LINK
ITO LINK_N4_SET  Move El1=RA_MA6       Exit=RA_MC_LUX
ITO LINK_N4_SOP  Move El1=Voca         Exit=RA_MC_OP
ITO LINK_N4_SE1  Move El1=ADD_LUMEN    Exit=RA_MC_E1
CLEAR LINK_N4_SE2 RA_MC_E2
ITO LINK_N4_SEX  Move El1=RA_LINK_REF  Exit=RA_MC_DEST
RVOCA LINK_N4_WIS WRITE_ITO_SLOTS
RREDI LINK_RET
/── LR: LR name src exit_lux → Move src→RA_SR_LUMEN; Voca SR_GLR→RA_LINK; Move RA_SR_REL→exit_lux
NEWREF LR LR_SPN
NOLINK
/Wire name: Move src → RA_SR_LUMEN
ITO LR_SPN    Move  El1=RA_MA0       Exit=RA_MC_LUX
ITO LR_SOP    Move  El1=Move         Exit=RA_MC_OP
ITO LR_SE1    Move  El1=RA_MA1       Exit=RA_MC_E1
CLEAR LR_SE2 RA_MC_E2
ITO LR_SEX    Move  El1=RA_SR_LUMEN_REF Exit=RA_MC_DEST
RVOCA LR_WIS  WRITE_ITO_SLOTS
/Alloc name_J anon: Voca SR_GLR→RA_LINK
ITO LR_N2     __LT_ALLOC_ITO
ITO LR_SJ     Move  El1=RA_MA_RET    Exit=RA_MC_J
/Wire name_J: Voca SR_GLR → RA_LINK
ITO LR_JOP    Move  El1=Voca         Exit=RA_MC_OP
ITO LR_JE1    Move  El1=SR_GLR       Exit=RA_MC_E1
CLEAR LR_JE2  RA_MC_E2
ITO LR_JEX    Move  El1=RA_LINK_REF  Exit=RA_MC_DEST
ITO LR_JLX    Move  El1=RA_MC_J      Exit=RA_MC_LUX
RVOCA LR_JWS  WRITE_ITO_SLOTS
/Link name→name_J via Next
ITO LR_NL     Add   El1=RA_MC_LUX   El2=C_5         Exit=RA_MC_SLOT
ITO LR_NLW    Write El1=RA_MC_SLOT   El2=RA_MC_J
/Alloc name_W anon: Move RA_SR_REL→exit_lux
ITO LR_N3     __LT_ALLOC_ITO
ITO LR_SW     Move  El1=RA_MA_RET    Exit=RA_MC_K
/Wire name_W: Move RA_SR_REL → exit_lux
ITO LR_WOP    Move  El1=Move          Exit=RA_MC_OP
ITO LR_WE1    Move  El1=RA_SR_REL_REF Exit=RA_MC_E1
CLEAR LR_WE2  RA_MC_E2
ITO LR_WEX    Move  El1=RA_MA2        Exit=RA_MC_DEST
ITO LR_WLX    Move  El1=RA_MC_K       Exit=RA_MC_LUX
RVOCA LR_WWS  WRITE_ITO_SLOTS
/Link name_J→name_W
ITO LR_JNL    Add   El1=RA_MC_J      El2=C_5         Exit=RA_MC_SLOT
ITO LR_JNLW   Write El1=RA_MC_SLOT   El2=RA_MC_K
/Update _last_ito = name_W
ITO LR_SL     Move  El1=RA_MC_K      Exit=RA_MC_PREV
RREDI LR_RET
/── LT: LT name src exit_lux → Move src→RA_SR_LUMEN; Voca SR_GLE→RA_LINK; Move RA_SR_OUT→exit_lux
NEWREF LT LT_SPN
NOLINK
/Wire name: Move src → RA_SR_LUMEN (same as LR but writes to RA_SR_LUMEN_REF)
ITO LT_SPN    Move  El1=RA_MA0       Exit=RA_MC_LUX
ITO LT_SOP    Move  El1=Move         Exit=RA_MC_OP
ITO LT_SE1    Move  El1=RA_MA1       Exit=RA_MC_E1
CLEAR LT_SE2 RA_MC_E2
ITO LT_SEX    Move  El1=RA_SR_LUMEN_REF Exit=RA_MC_DEST
RVOCA LT_WIS  WRITE_ITO_SLOTS
ITO LT_N2     __LT_ALLOC_ITO
ITO LT_SJ     Move  El1=RA_MA_RET    Exit=RA_MC_J
/Wire name_J: Voca SR_GLE → RA_LINK
ITO LT_JOP    Move  El1=Voca         Exit=RA_MC_OP
ITO LT_JE1    Move  El1=SR_GLE       Exit=RA_MC_E1
CLEAR LT_JE2  RA_MC_E2
ITO LT_JEX    Move  El1=RA_LINK_REF  Exit=RA_MC_DEST
ITO LT_JLX    Move  El1=RA_MC_J      Exit=RA_MC_LUX
RVOCA LT_JWS  WRITE_ITO_SLOTS
ITO LT_NL     Add   El1=RA_MC_LUX   El2=C_5         Exit=RA_MC_SLOT
ITO LT_NLW    Write El1=RA_MC_SLOT   El2=RA_MC_J
ITO LT_N3     __LT_ALLOC_ITO
ITO LT_SW     Move  El1=RA_MA_RET    Exit=RA_MC_K
/Wire name_W: Move RA_SR_OUT → exit_lux
ITO LT_WOP    Move  El1=Move          Exit=RA_MC_OP
ITO LT_WE1    Move  El1=RA_SR_OUT_REF Exit=RA_MC_E1
CLEAR LT_WE2  RA_MC_E2
ITO LT_WEX    Move  El1=RA_MA2        Exit=RA_MC_DEST
ITO LT_WLX    Move  El1=RA_MC_K       Exit=RA_MC_LUX
RVOCA LT_WWS  WRITE_ITO_SLOTS
ITO LT_JNL    Add   El1=RA_MC_J      El2=C_5         Exit=RA_MC_SLOT
ITO LT_JNLW   Write El1=RA_MC_SLOT   El2=RA_MC_K
ITO LT_SL     Move  El1=RA_MC_K      Exit=RA_MC_PREV
RREDI LT_RET
/── WALK_ITO: WALK_ITO name lux rel → like WALK_ONE but offset=C_7 (ITO_SIZE)
NEWREF WALK_ITO WALK_ITO_SPN
NOLINK
/Wire name: Move src → RA_SR_LUX
ITO WALK_ITO_SPN  Move  El1=RA_MA0       Exit=RA_MC_LUX
ITO WALK_ITO_SOP  Move  El1=Move         Exit=RA_MC_OP
ITO WALK_ITO_SE1  Move  El1=RA_MA1       Exit=RA_MC_E1
CLEAR WALK_ITO_SE2 RA_MC_E2
ITO WALK_ITO_SEX  Move  El1=RA_SR_LUX_REF Exit=RA_MC_DEST
RVOCA WALK_ITO_W1 WRITE_ITO_SLOTS
/Wire name_REL: Move rel → RA_SR_REL
ITO WALK_ITO_N2   __LT_ALLOC_ITO
ITO WALK_ITO_SJ   Move  El1=RA_MA_RET    Exit=RA_MC_J
ITO WALK_ITO_NLI  Add   El1=RA_MC_LUX   El2=C_5         Exit=RA_MC_SLOT
ITO WALK_ITO_NLIW Write El1=RA_MC_SLOT   El2=RA_MC_J
ITO WALK_ITO_JOP  Move  El1=Move         Exit=RA_MC_OP
ITO WALK_ITO_JE1  Move  El1=RA_MA2       Exit=RA_MC_E1
CLEAR WALK_ITO_JE2 RA_MC_E2
ITO WALK_ITO_JEX  Move  El1=RA_SR_REL_REF Exit=RA_MC_DEST
ITO WALK_ITO_JLX  Move  El1=RA_MC_J      Exit=RA_MC_LUX
RVOCA WALK_ITO_W2 WRITE_ITO_SLOTS
/Wire name_OFF: Move C_7 → RA_SR_OFFSET
ITO WALK_ITO_N3   __LT_ALLOC_ITO
ITO WALK_ITO_SK   Move  El1=RA_MA_RET    Exit=RA_MC_K
ITO WALK_ITO_JNLI Add   El1=RA_MC_J      El2=C_5         Exit=RA_MC_SLOT
ITO WALK_ITO_JNLIW Write El1=RA_MC_SLOT  El2=RA_MC_K
ITO WALK_ITO_KOP  Move  El1=Move         Exit=RA_MC_OP
ITO WALK_ITO_KE1  Move  El1=C_7          Exit=RA_MC_E1
CLEAR WALK_ITO_KE2 RA_MC_E2
ITO WALK_ITO_KEX  Move  El1=RA_SR_OFFSET_REF Exit=RA_MC_DEST
ITO WALK_ITO_KLX  Move  El1=RA_MC_K      Exit=RA_MC_LUX
RVOCA WALK_ITO_W3 WRITE_ITO_SLOTS
/Wire name: Voca SR_WALK_ONE Exit=RA_LINK
ITO WALK_ITO_N4   __LT_ALLOC_ITO
ITO WALK_ITO_S4   Move  El1=RA_MA_RET    Exit=RA_MC_LUX
ITO WALK_ITO_KNLI Add   El1=RA_MC_K      El2=C_5         Exit=RA_MC_SLOT
ITO WALK_ITO_KNLIW Write El1=RA_MC_SLOT  El2=RA_MC_LUX
ITO WALK_ITO_VOP  Move  El1=Voca         Exit=RA_MC_OP
ITO WALK_ITO_VE1  Move  El1=SR_WALK_ONE  Exit=RA_MC_E1
CLEAR WALK_ITO_VE2 RA_MC_E2
ITO WALK_ITO_VEX  Move  El1=RA_LINK_REF  Exit=RA_MC_DEST
RVOCA WALK_ITO_W4 WRITE_ITO_SLOTS
RREDI WALK_ITO_RET
/── UNLINK_OP: UNLINK_OP name src rel exit → like LINK_OP but REMOVE_LUMEN
/MA0=name MA1=src MA2=rel MA3=exit_lux MA4=name_R(auto) MA5=name_T(auto) MA6=name_J(auto)
/Identical structure to LINK_OP; only E1 of name_J differs (REMOVE_LUMEN vs ADD_LUMEN).
NEWREF UNLINK_OP UNLINK_OP_N1
NOLINK
/Wire name: Move src → RA_LM_SRC
ITO UNLINK_OP_N1  Move El1=RA_MA0       Exit=RA_MC_LUX
ITO UO_N1_SOP     Move El1=Move         Exit=RA_MC_OP
ITO UO_N1_SE1     Move El1=RA_MA1       Exit=RA_MC_E1
CLEAR UO_N1_SE2   RA_MC_E2
ITO UO_N1_SEX     Move El1=RA_LM_SRC    Exit=RA_MC_DEST
RVOCA UO_N1_WIS   WRITE_ITO_SLOTS
/Wire name_R: Move rel → RA_LM_REL
ITO UO_N2_SET     Move El1=RA_MA4       Exit=RA_MC_LUX
ITO UO_N2_SOP     Move El1=Move         Exit=RA_MC_OP
ITO UO_N2_SE1     Move El1=RA_MA2       Exit=RA_MC_E1
CLEAR UO_N2_SE2   RA_MC_E2
ITO UO_N2_SEX     Move El1=RA_LM_REL    Exit=RA_MC_DEST
RVOCA UO_N2_WIS   WRITE_ITO_SLOTS
/Wire name_T: Move exit → RA_LM_EXIT
ITO UO_N3_SET     Move El1=RA_MA5       Exit=RA_MC_LUX
ITO UO_N3_SOP     Move El1=Move         Exit=RA_MC_OP
ITO UO_N3_SE1     Move El1=RA_MA3       Exit=RA_MC_E1
CLEAR UO_N3_SE2   RA_MC_E2
ITO UO_N3_SEX     Move El1=RA_LM_EXIT   Exit=RA_MC_DEST
RVOCA UO_N3_WIS   WRITE_ITO_SLOTS
/Wire name_J: Voca REMOVE_LUMEN Exit=RA_LINK
ITO UO_N4_SET     Move El1=RA_MA6       Exit=RA_MC_LUX
ITO UO_N4_SOP     Move El1=Voca         Exit=RA_MC_OP
ITO UO_N4_SE1     Move El1=REMOVE_LUMEN Exit=RA_MC_E1
CLEAR UO_N4_SE2   RA_MC_E2
ITO UO_N4_SEX     Move El1=RA_LINK_REF  Exit=RA_MC_DEST
RVOCA UO_N4_WIS   WRITE_ITO_SLOTS
RREDI UNLINK_OP_RET
/── RCALL_AT: RCALL_AT name sub landing → Move landing→RA_LINK; Jump→sub. Resets flow.
/MA0=name_addr MA1=sub_addr MA2=landing_addr
NEWREF RCALL_AT RCALL_AT_SPN
NOLINK
/Wire name: Move landing → RA_LINK
ITO RCALL_AT_SPN  Move  El1=RA_MA0       Exit=RA_MC_LUX
ITO RCA_N1_OP     Move  El1=Move         Exit=RA_MC_OP
ITO RCA_N1_E1     Move  El1=RA_MA2       Exit=RA_MC_E1
CLEAR RCA_N1_E2   RA_MC_E2
ITO RCA_N1_EX     Move  El1=RA_LINK_REF  Exit=RA_MC_DEST
RVOCA RCA_W1      WRITE_ITO_SLOTS
/Link name→name_J; Wire name_J: Jump Exit=sub (chain terminator)
ITO RCALL_AT_NL   Add   El1=RA_MC_LUX   El2=C_5 Exit=RA_MC_SLOT
ITO RCALL_AT_N2   __LT_ALLOC_ITO
ITO RCALL_AT_SJ   Move  El1=RA_MA_RET    Exit=RA_MC_J
ITO RCALL_AT_NLW  Write El1=RA_MC_SLOT   El2=RA_MC_J
ITO RCALL_AT_JN   Move  El1=RA_MC_J      Exit=RA_MC_LUX
ITO RCA_N2_OP     Move  El1=Jump         Exit=RA_MC_OP
CLEAR RCA_N2_E1   RA_MC_E1
CLEAR RCA_N2_E2   RA_MC_E2
ITO RCA_N2_EX     Move  El1=RA_MA1       Exit=RA_MC_DEST
RVOCA RCA_W2      WRITE_ITO_SLOTS_RESET
RREDI RCALL_AT_RET
/── RCALL/RRET: REMOVED ──────────────────────────────────────────────────
/These macros implemented an inline call stack using a fixed 1024-entry
/array (BS_CS_BUF_000/BS_CS_SP in bootstrap.re). Zero callers remain in
/the project — the automatic call stack (Voca/Redi push/pop RA_LINK on
/RA_SP, see runtime/regs.re) replaced this entirely. BS_CS_BUF_000/BS_CS_SP
/have also been removed from bootstrap.re.
/── RCN_IMPL: shared body for EMIT/EMITI/PUTBYTE ────────────────────────────
/IN: RA_MC_LUX=name_addr, RA_MA1=arg_addr,
/    RA_MC_OP=fn (EMIT_STR_ENTRY|EMIT_INT_ENTRY|PUT_BYTE),
/    RA_MC_DEST=reg_ref (RA_TW_LUX_REF|RA_TMP2_REF|RA_BYTE_REF).
/Wires: name(Move arg→reg_ref) → anon_J(Voca fn) → anon_K(NOP). Sets RA_MC_PREV=_K.
NEWREF RCN_IMPL RCN_S0
NOLINK
ITO RCN_S0     Move  El1=RA_MC_LUX   Exit=RA_MC_SLOT
ITO RCN_W0     Write El1=RA_MC_SLOT   El2=RA_MC_LUX
ITO RCN_A1     Add   El1=RA_MC_LUX   El2=C_1         Exit=RA_MC_SLOT
ITO RCN_W1     Write El1=RA_MC_SLOT   El2=Move
ITO RCN_A2     Add   El1=RA_MC_LUX   El2=C_2         Exit=RA_MC_SLOT
ITO RCN_W2     Write El1=RA_MC_SLOT   El2=RA_MA1
ITO RCN_A4     Add   El1=RA_MC_LUX   El2=C_4         Exit=RA_MC_SLOT
ITO RCN_W4     Write El1=RA_MC_SLOT   El2=RA_MC_DEST
ITO RCN_SRL    Move  El1=RA_LINK      Exit=RA_MC_TMP_RL
ITO RCN_CAL    Voca  El1=AUTOLINK     Exit=RA_LINK
NOLINK
ITO RCN_RST    Move  El1=RA_MC_TMP_RL Exit=RA_LINK
ITO RCN_N2     __LT_ALLOC_ITO
ITO RCN_SJ     Move  El1=RA_MA_RET    Exit=RA_MC_J
ITO RCN_NL     Add   El1=RA_MC_LUX   El2=C_5         Exit=RA_MC_SLOT
ITO RCN_NLW    Write El1=RA_MC_SLOT   El2=RA_MC_J
ITO RCN_J0     Move  El1=RA_MC_J      Exit=RA_MC_SLOT
ITO RCN_JW0    Write El1=RA_MC_SLOT   El2=RA_MC_J
ITO RCN_JA1    Add   El1=RA_MC_J      El2=C_1         Exit=RA_MC_SLOT
ITO RCN_JW1    Write El1=RA_MC_SLOT   El2=Voca
ITO RCN_JA2    Add   El1=RA_MC_J      El2=C_2         Exit=RA_MC_SLOT
ITO RCN_JW2    Write El1=RA_MC_SLOT   El2=RA_MC_OP
ITO RCN_JA4    Add   El1=RA_MC_J      El2=C_4         Exit=RA_MC_SLOT
ITO RCN_JW4    Write El1=RA_MC_SLOT   El2=RA_LINK_REF
ITO RCN_N3     __LT_ALLOC_ITO
ITO RCN_SK     Move  El1=RA_MA_RET    Exit=RA_MC_K
ITO RCN_JNL    Add   El1=RA_MC_J      El2=C_5         Exit=RA_MC_SLOT
ITO RCN_JNLW   Write El1=RA_MC_SLOT   El2=RA_MC_K
ITO RCN_K0     Move  El1=RA_MC_K      Exit=RA_MC_SLOT
ITO RCN_KW0    Write El1=RA_MC_SLOT   El2=RA_MC_K
ITO RCN_KA1    Add   El1=RA_MC_K      El2=C_1         Exit=RA_MC_SLOT
ITO RCN_KW1    Write El1=RA_MC_SLOT   El2=Move
ITO RCN_KA2    Add   El1=RA_MC_K      El2=C_2         Exit=RA_MC_SLOT
ITO RCN_KW2    Write El1=RA_MC_SLOT   El2=C_0
ITO RCN_KA4    Add   El1=RA_MC_K      El2=C_4         Exit=RA_MC_SLOT
ITO RCN_KW4    Write El1=RA_MC_SLOT   El2=C_0
ITO RCN_SL     Move  El1=RA_MC_K      Exit=RA_MC_PREV
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
ITO ITO_LADD        Add   El1=RA_MC_PREV   El2=C_5           Exit=RA_MC_SLOT
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
ITO NEWSET_START    Write El1=RA_MA0       El2=RA_MA1
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
/EOF or non-indented → done
ITO SWITCH_EOFCK     Equal El1=RA_LOAD_BYTE  El2=C_0          Exit=RA_BS_FLAG
ITO SWITCH_EOFJ      JumpIf El1=RA_BS_FLAG   Exit=SWITCH_DONE
ITO SWITCH_INDCK     Equal El1=LD_INDENT_DEPTH El2=C_0        Exit=RA_BS_FLAG
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
ITO SWITCH_T1EJ      JumpIf El1=RA_BS_FLAG   Exit=SWITCH_LINE
RVOCA SWITCH_T1I     LOAD_INTERN_TOKEN
ITO SWITCH_T1SV      Move  El1=RA_LOAD_RESULT Exit=RA_MA2
/Peek: is second token '>' or another val or dest?
RVOCA SWITCH_T2      LOAD_READ_TOKEN
ITO SWITCH_T2EK      Equal El1=RA_LOAD_TLEN  El2=C_0          Exit=RA_BS_FLAG
ITO SWITCH_T2EJ      JumpIf El1=RA_BS_FLAG   Exit=SWITCH_EMIT1
/Check if this token is '>'
ITO SWITCH_T2B0      Read  El1=BS_TOKBUF_BASE Exit=RA_BS_TMP3
ITO SWITCH_T2ARR     Equal El1=RA_BS_TMP3    El2=SW_ARROW_BYTE Exit=RA_BS_FLAG
ITO SWITCH_T2ARRJ    JumpIf El1=RA_BS_FLAG   Exit=SWITCH_READ_DEST_AFTER_1
RVOCA SWITCH_T2I     LOAD_INTERN_TOKEN
ITO SWITCH_T2SV      Move  El1=RA_LOAD_RESULT Exit=RA_MA3
/Read third token
RVOCA SWITCH_T3      LOAD_READ_TOKEN
ITO SWITCH_T3EK      Equal El1=RA_LOAD_TLEN  El2=C_0          Exit=RA_BS_FLAG
ITO SWITCH_T3EJ      JumpIf El1=RA_BS_FLAG   Exit=SWITCH_EMIT2
ITO SWITCH_T3B0      Read  El1=BS_TOKBUF_BASE Exit=RA_BS_TMP3
ITO SWITCH_T3ARR     Equal El1=RA_BS_TMP3    El2=SW_ARROW_BYTE Exit=RA_BS_FLAG
ITO SWITCH_T3ARRJ    JumpIf El1=RA_BS_FLAG   Exit=SWITCH_READ_DEST_AFTER_2
RVOCA SWITCH_T3I     LOAD_INTERN_TOKEN
ITO SWITCH_T3SV      Move  El1=RA_LOAD_RESULT Exit=RA_MA4
/Read fourth token (should be '>' or dest)
RVOCA SWITCH_T4      LOAD_READ_TOKEN
ITO SWITCH_T4EK      Equal El1=RA_LOAD_TLEN  El2=C_0          Exit=RA_BS_FLAG
ITO SWITCH_T4EJ      JumpIf El1=RA_BS_FLAG   Exit=SWITCH_EMIT3
ITO SWITCH_T4B0      Read  El1=BS_TOKBUF_BASE Exit=RA_BS_TMP3
ITO SWITCH_T4ARR     Equal El1=RA_BS_TMP3    El2=SW_ARROW_BYTE Exit=RA_BS_FLAG
ITO SWITCH_T4ARRJ    JumpIf El1=RA_BS_FLAG   Exit=SWITCH_READ_DEST_AFTER_3
/Four values: MA2/MA3/MA4/MA5
RVOCA SWITCH_T4I     LOAD_INTERN_TOKEN
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
ITO SWITCH_E1_JMP    Jump  Exit=SWITCH_LINE
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
ITO SWITCH_E2_JMP    Jump  Exit=SWITCH_LINE
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
ITO SWITCH_E3_JMP    Jump  Exit=SWITCH_LINE
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
ITO SWITCH_E4_JMP    Jump  Exit=SWITCH_LINE
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
JZ SW_RN_ANON RA_SW_BASE SW_RN_ANON_PATH
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
/── Stubs for LOAD_* subroutines (implemented in saku.re) ──
/These are forward references — saku.re defines them.
/Declared here so macros.re compiles without forward-ref errors.
NEWREF LOAD_READ_LINE       LOAD_READ_LINE_STUB
NEWREF LOAD_READ_TOKEN      LOAD_READ_TOKEN_STUB
NEWREF LOAD_INTERN_TOKEN    LOAD_INTERN_TOKEN_STUB
NEWREF LOAD_READ_BODY       LOAD_READ_BODY_STUB
NEWREF LOAD_EXPAND_TEMPLATE LOAD_EXPAND_TEMPLATE_STUB
NEWREF LOAD_PROCESS_BODY    LOAD_PROCESS_BODY_STUB
NEWREF SAVE_EMIT_SAVES      SAVE_EMIT_SAVES_STUB
NEWREF SAVE_EMIT_RESTORES   SAVE_EMIT_RESTORES_STUB

/Stub implementations (NOP — replaced when saku.re loads)
NOITO
    LOAD_READ_LINE_STUB       Move El1=C_0 Exit=RA_LOAD_BYTE
        RREDI LOAD_RL_STUB_R
    LOAD_READ_TOKEN_STUB      Move El1=C_0 Exit=RA_LOAD_TLEN
        RREDI LOAD_RT_STUB_R
    LOAD_INTERN_TOKEN_STUB    Move El1=C_0 Exit=RA_LOAD_RESULT
        RREDI LOAD_IT_STUB_R
    LOAD_READ_BODY_STUB       Move El1=C_0 Exit=RA_LOAD_RESULT
        RREDI LOAD_RB_STUB_R
    LOAD_EXPAND_TEMPLATE_STUB Move El1=C_0 Exit=RA_LOAD_RESULT
        RREDI LOAD_ET_STUB_R
    LOAD_PROCESS_BODY_STUB    Move El1=C_0 Exit=RA_LOAD_RESULT
        RREDI LOAD_PB_STUB_R
    SAVE_EMIT_SAVES_STUB      Move El1=C_0 Exit=RA_LOAD_RESULT
        RREDI SAVE_ES_STUB_R
    SAVE_EMIT_RESTORES_STUB   Move El1=C_0 Exit=RA_LOAD_RESULT
        RREDI SAVE_ER_STUB_R
/============================================================
/NEXO and YAKU_NEXO — build a packed string from a template
/
/NEXO name "template"
/  MA0=name_addr, MA1=addr_of_quoted_string_in_tokbuf
/  Reads bytes from BS_TOKBUF (token with quotes already consumed).
/  Builds packed string via PGB_EMIT_BYTE/PGB_FLUSH_WORD/PGB_FINALIZE.
/  Writes PGB_PS_FIRST into aether[name_addr].
/
/YAKU_NEXO name
/  MA0=name_addr. Reads indented body lines via LOAD_READ_LINE.
/  Each body line = one template line. Placeholders work as usual.
/============================================================

/── NEXO_MACRO: NEXO name "template" ─────────────────────────
/MA0=name_addr, MA1 unused (name is in MA0, template in tokbuf after token read)
/Reads template from current file position (next token should be "string")
NEWREF NEXO NEXO_START
NOLINK
/Init packed string builder state
ITO NEXO_START       Move  El1=C_0          Exit=PGB_PS_FIRST
ITO NEXO_PS_INIT1    Move  El1=C_0          Exit=PGB_PS_WORD
ITO NEXO_PS_INIT2    Move  El1=C_0          Exit=PGB_PS_SHIFT
/Read template token from file (should be quoted string or bare token)
RVOCA NEXO_RTOK      BS_READ_TOKEN
/Scan token byte by byte through PGB_EMIT_BYTE
/The token is in BS_TOKBUF_BASE, length in RA_LOAD_TLEN
ITO NEXO_SIDX        Move  El1=C_0          Exit=RA_BS_TMP3
ITO NEXO_LOOP        Equal El1=RA_BS_TMP3   El2=RA_LOAD_TLEN  Exit=RA_BS_FLAG
ITO NEXO_LCKJ        JumpIf El1=RA_BS_FLAG  Exit=NEXO_DONE_BUILD
ITO NEXO_BADDR       Add   El1=BS_TOKBUF_BASE El2=RA_BS_TMP3  Exit=RA_BS_TMP
ITO NEXO_RBYTE       Read  El1=RA_BS_TMP    Exit=RA_TMP
/Check for '{' → placeholder
ITO NEXO_LBCK        Equal El1=RA_TMP       El2=LBRACE Exit=RA_BS_FLAG
ITO NEXO_LBCJ        JumpIf El1=RA_BS_FLAG  Exit=NEXO_PLACEHOLDER
/Ordinary byte → emit
RVOCA NEXO_EB        PGB_EMIT_BYTE
ITO NEXO_INC         Add   El1=RA_BS_TMP3   El2=C_1           Exit=RA_BS_TMP3
ITO NEXO_JMP         Jump  Exit=NEXO_LOOP
/Placeholder: scan until '}', call PGB_PLACEHOLDER
NOLINK
ITO NEXO_PLACEHOLDER Move  El1=RA_BS_TMP3   Exit=PR_LPOS
ITO NEXO_PLACEHOLDER2 Move El1=RA_LOAD_TLEN Exit=PR_LLEN
ITO NEXO_PLACEHOLDER3 Move El1=BS_TOKBUF_BASE Exit=PR_LBUF
RVOCA NEXO_PHCALL    PGB_PLACEHOLDER
/Advance past placeholder (PGB_PLACEHOLDER consumed bytes via PR_LPOS)
ITO NEXO_PLADV       Move  El1=PR_LPOS      Exit=RA_BS_TMP3
ITO NEXO_PLJMP       Jump  Exit=NEXO_LOOP
/Done: finalize + write result to name lux
NOLINK
RVOCA NEXO_DONE_BUILD   PGB_FINALIZE
/aether[name_addr] = PGB_PS_FIRST (first packed string lux)
ITO NEXO_WRITE       Write El1=RA_MA0       El2=PGB_PS_FIRST
RVOCA NEXO_SKIP      BS_SKIP_TO_EOL
RREDI NEXO_RRET
/── YAKU_NEXO_MACRO: YAKU_NEXO name (indented body) ──────────
/MA0=name_addr. Reads indented body lines as template bytes.
/Each body line: bytes emitted literally with {X} → escape byte.
/After all lines: finalize + write result.
NEWREF YAKU_NEXO YAKU_NEXO_START
NOLINK
/Init packed string builder
ITO YAKU_NEXO_START  Move  El1=C_0          Exit=PGB_PS_FIRST
ITO SN_INIT1         Move  El1=C_0          Exit=PGB_PS_WORD
ITO SN_INIT2         Move  El1=C_0          Exit=PGB_PS_SHIFT
RVOCA SN_SKP         BS_SKIP_TO_EOL
/Read body lines until non-indented or EOF
RVOCA SN_LINE_LOOP   LOAD_READ_LINE
/EOF or non-indented → done
ITO SN_EOFCK         Equal El1=RA_LOAD_BYTE  El2=C_0           Exit=RA_BS_FLAG
ITO SN_EOFJ          JumpIf El1=RA_BS_FLAG   Exit=SN_DONE_BUILD
ITO SN_INDCK         Equal El1=LD_INDENT_DEPTH El2=C_0         Exit=RA_BS_FLAG
ITO SN_INDJ          JumpIf El1=RA_BS_FLAG   Exit=SN_DONE_BUILD
/Emit LF between lines (except first)
ITO SN_LFCK          Equal El1=PGB_PS_FIRST  El2=C_0           Exit=RA_BS_FLAG
ITO SN_LFCKJ         JumpIf El1=RA_BS_FLAG   Exit=SN_READ_LINE_BYTES
ITO SN_LF_EMIT       Move  El1=LF            Exit=RA_TMP
RVOCA SN_LFEB        PGB_EMIT_BYTE
/Read and emit bytes of this line
RVOCA SN_READ_LINE_BYTES   BS_READ_BYTE
ITO SN_BEOF          Equal El1=RA_LOAD_BYTE  El2=C_0           Exit=RA_BS_FLAG
ITO SN_BEOFJ         JumpIf El1=RA_BS_FLAG   Exit=SN_LINE_LOOP
ITO SN_BLF           Equal El1=RA_LOAD_BYTE  El2=LF         Exit=RA_BS_FLAG
ITO SN_BLFJ          JumpIf El1=RA_BS_FLAG   Exit=SN_LINE_LOOP
/Check for '{'
ITO SN_LBCK          Equal El1=RA_LOAD_BYTE  El2=LBRACE Exit=RA_BS_FLAG
ITO SN_LBCJ          JumpIf El1=RA_BS_FLAG   Exit=SN_PLACEHOLDER
/Ordinary byte
ITO SN_EBYTE         Move  El1=RA_LOAD_BYTE  Exit=RA_TMP
RVOCA SN_EB          PGB_EMIT_BYTE
ITO SN_BJMP          Jump  Exit=SN_READ_LINE_BYTES
/Placeholder: read name bytes into tokbuf, call PGB_PLACEHOLDER
NOLINK
ITO SN_PLACEHOLDER   Move  El1=C_0          Exit=PR_LPOS
ITO SN_PLBUF         Move  El1=BS_TOKBUF_BASE Exit=PR_LBUF
RVOCA SN_PL_RBYTE     BS_READ_BYTE
ITO SN_PL_RBCK       Equal El1=RA_LOAD_BYTE  El2=RBRACE Exit=RA_BS_FLAG
ITO SN_PL_RBJ        JumpIf El1=RA_BS_FLAG   Exit=SN_PL_CALL
ITO SN_PL_STORE      Add   El1=BS_TOKBUF_BASE El2=PR_LPOS      Exit=RA_BS_TMP
ITO SN_PL_SW         Write El1=RA_BS_TMP     El2=RA_LOAD_BYTE
ITO SN_PL_INC        Add   El1=PR_LPOS       El2=C_1           Exit=PR_LPOS
ITO SN_PL_JMP        Jump  Exit=SN_PL_RBYTE
ITO SN_PL_CALL       Move  El1=PR_LPOS       Exit=PR_LLEN
RVOCA SN_PL_PHCALL   PGB_PLACEHOLDER
ITO SN_PL_BJMP       Jump  Exit=SN_READ_LINE_BYTES
/Done: finalize + write
NOLINK
RVOCA SN_DONE_BUILD   PGB_FINALIZE
ITO SN_WRITE         Write El1=RA_MA0        El2=PGB_PS_FIRST
RREDI SN_RRET
/── SNL_IMPL: shared body for YAKU_NEXO_TERM/CMP/ARITH ───────────────────────
/Caller sets before RVOCA: RA_LM_REL = relation to attach.
/Runs YAKU_NEXO, then ADD_LUMEN(MA0, RA_LM_REL, Yaku).
NEWREF SNL_IMPL SNL_IMPL_START
NOLINK
RVOCA SNL_IMPL_START      YAKU_NEXO
ITO SNL_SRC               Move  El1=RA_MA0        Exit=RA_LM_SRC
/RA_LM_REL already set by caller
ITO SNL_TGT               Move  El1=Yaku           Exit=RA_LM_EXIT
RVOCA SNL_ADD             ADD_LUMEN
RREDI SNL_RET
/── YAKU_NEXO_TERM: YAKU_NEXO + LINK name Terminates Yaku ────
/MA0=name_addr. Sets relation then jumps directly into SNL_IMPL (no extra frame).
NEWREF YAKU_NEXO_TERM YAKU_NEXO_TERM_REL
NOLINK
ITO YAKU_NEXO_TERM_REL    Move  El1=Terminates     Exit=RA_LM_REL
ITO YAKU_NEXO_TERM_JMP    Jump  Exit=SNL_IMPL

/── YAKU_NEXO_CMP: YAKU_NEXO + LINK name HasCmpResult Yaku ────
/MA0=name_addr. EMIT_BLOCK detects HasCmpResult → saves RA_SSA_FR for fusion.
NEWREF YAKU_NEXO_CMP YAKU_NEXO_CMP_REL
NOLINK
ITO YAKU_NEXO_CMP_REL     Move  El1=HasCmpResult   Exit=RA_LM_REL
ITO YAKU_NEXO_CMP_JMP     Jump  Exit=SNL_IMPL

/── YAKU_NEXO_ARITH: YAKU_NEXO + LINK name HasArithResult Yaku ─
/MA0=name_addr. EMIT_BLOCK detects HasArithResult → saves RA_SSA_RESULT for fusion.
NEWREF YAKU_NEXO_ARITH YAKU_NEXO_ARITH_REL
NOLINK
ITO YAKU_NEXO_ARITH_REL   Move  El1=HasArithResult Exit=RA_LM_REL
ITO YAKU_NEXO_ARITH_JMP   Jump  Exit=SNL_IMPL

/── YAKU_NEXO_ALIAS: YAKU_NEXO_TERM + ForType lumen from alias ─
/MA0=name_addr MA1=alias_addr.
/Does YAKU_NEXO_TERM for name, then adds lumen (ForType→name) to alias lux.
NEWREF YAKU_NEXO_ALIAS YAKU_NEXO_ALIAS_START
NOLINK
/Read alias token (second arg after name)
RVOCA YAKU_NEXO_ALIAS_START   BS_READ_TOKEN
RVOCA SNL_AINT            BS_INTERN
ITO SNL_SAVE_ALIAS        Move  El1=RA_BS_RESULT  Exit=RA_MA1
/Do YAKU_NEXO_TERM for the primary name
RVOCA SNL_DO_TERM         YAKU_NEXO_TERM
/Add lumen (ForType → name) to alias lux (MA1)
ITO SNL_SRC               Move  El1=RA_MA1        Exit=RA_LM_SRC
ITO SNL_REL               Move  El1=ForType        Exit=RA_LM_REL
ITO SNL_TGT               Move  El1=RA_MA0         Exit=RA_LM_EXIT
RVOCA SNL_ADD             ADD_LUMEN
RREDI SNL_RRET
/── FUNC/ENDFUNC: REMOVED ──────────────────────────────────────────────────
/These macros generated the old manual RA_LINK save/restore pattern
/(push via CS_PUSH, pop via CS_POP). Zero callers remain in the project —
/the automatic call stack (Voca/Redi push/pop RA_LINK on RA_SP) makes this
/pattern fully redundant. See runtime/regs.re for the current convention.

/── NOITO: list of independent luces, each with its own NOLINK ────────────────
/Three modes (MA0 = block name, 0 if none):
/  NOITO          MA0=0 → explicit names (NOITO_COPY_LINE_ITO)
/  NOITO _        MA0=lux("_") → fully anonymous (__ni_N via NOITO_BUILD_ANON)
/  NOITO NAME     MA0=lux(NAME) → auto-named (IRIS_N via NOITO_BUILD_NAME)
/
/indent=1: new independent lux (NOLINK + ITO)
/indent=2: sub-lux dispatch (RVOCA/RREDI/etc)
NEWREF NOITO NOITO_START
NEW LD_NI_IDX        /NOITO NAME: auto-name counter (0-based)
NEW LD_NI_BASE       /NOITO NAME: base name addr (MA0, 0 if anon/manual)
NEW LD_NI_FULLY_ANON /NOITO _: 1 if fully anonymous
NOLINK
CLEAR NOITO_START    LD_NI_IDX
CLEAR NOITO_FANON_CL LD_NI_FULLY_ANON
ITO NOITO_SAVBASE    Move  El1=RA_MA0      Exit=LD_NI_BASE
/Detect NOITO _ (MA0 = lux for "_", packed word = UNDERSCORE = 95)
JZ NOITO_BASENIL LD_NI_BASE NOITO_RDBODY
ITO NOITO_BASEWORD   Read  El1=LD_NI_BASE  Exit=LD_NI_MARK
JEQ NOITO_FANON_CK   LD_NI_MARK UNDERSCORE NOITO_SET_FANON
ITO NOITO_FANON_SKIP Jump  Exit=NOITO_RDBODY
NOLINK
ITO NOITO_SET_FANON  Move  El1=C_1         Exit=LD_NI_FULLY_ANON
NOLINK
RVOCA NOITO_RDBODY   LOAD_READ_BODY
/Walk body buffer: each line prefixed with marker byte (C_1 or C_2)
ITO NOITO_WALK_INIT  Move  El1=LD_BODY_BUF_BASE_VAL  Exit=LD_NI_PTR
ITO NOITO_WALK_LOOP  Equal El1=LD_NI_PTR  El2=LD_BODY_PTR  Exit=LD_FLAG
ITO NOITO_WALK_LCKJ  JumpIf El1=LD_FLAG  Exit=NOITO_DONE
/Read marker byte
READ_BODY NOITO_MARK_RD LD_NI_MARK
/marker=C_1 → new independent lux
JEQ NOITO_MRK1CK     LD_NI_MARK C_1 NOITO_NEW_LUX
/marker=C_2 → sub-lux: pass through unchanged
RVOCA NOITO_SUB_COPY NOITO_COPY_LINE
RVOCA NOITO_SUB_DISP LOAD_DISPATCH_LINE
ITO NOITO_SUB_JMP    Jump  Exit=NOITO_WALK_LOOP
/New lux: NOLINK, then dispatch by mode
NOLINK
CLEAR NOITO_NEW_LUX  RA_MC_PREV
/Check mode: anon / named / manual
JEQ NOITO_FANON_D    LD_NI_FULLY_ANON C_1 NOITO_MODE_ANON
JZ  NOITO_BASE_CK    LD_NI_BASE NOITO_MODE_MANUAL
/Named mode: NOITO NAME → auto-name IRIS_N
RVOCA NOITO_MODE_NAMED  NOITO_BUILD_NAME
RVOCA NOITO_NAMED_DISP  LOAD_DISPATCH_LINE
ITO NOITO_NAMED_IDX  Add   El1=LD_NI_IDX  El2=C_1  Exit=LD_NI_IDX
ITO NOITO_NAMED_JMP  Jump  Exit=NOITO_WALK_LOOP
/Anon mode: NOITO _ → __ni_N
NOLINK
RVOCA NOITO_MODE_ANON   NOITO_BUILD_ANON_NAME
RVOCA NOITO_ANON_DISP   LOAD_DISPATCH_LINE
ITO NOITO_ANON_IDX   Add   El1=LD_NI_IDX  El2=C_1  Exit=LD_NI_IDX
ITO NOITO_ANON_JMP   Jump  Exit=NOITO_WALK_LOOP
/Manual mode: NOITO → explicit names (prepend "ITO ")
NOLINK
RVOCA NOITO_MODE_MANUAL NOITO_COPY_LINE_ITO
RVOCA NOITO_MAN_DISP    LOAD_DISPATCH_LINE
ITO NOITO_MAN_JMP    Jump  Exit=NOITO_WALK_LOOP
NOLINK
RREDI NOITO_DONE
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
NEW LD_NI_PTR        /walk pointer in body buffer (shared with NOITO)
NEW LD_NI_MARK       /current marker byte
NEW LD_CH_IDX        /CHAIN: auto-name counter (0-based)
NEW LD_CH_BASE       /CHAIN: base name addr (MA0)
NEW LD_CH_FULLY_ANON /CHAIN _: 1 if fully anonymous (__ch_N naming), 0 otherwise
NOLINK
/Single NOLINK: clear RA_MC_PREV
CLEAR CHAIN_START        RA_MC_PREV
CLEAR CHAIN_IDX          LD_CH_IDX
CLEAR CHAIN_FANON        LD_CH_FULLY_ANON
ITO CHAIN_SAVBASE        Move  El1=RA_MA0      Exit=LD_CH_BASE
/Check if CHAIN _ (MA0 = lux for "_"): detect by reading packed word = 0x5F (95)
JZ CHAIN_BASENIL LD_CH_BASE CHAIN_RDBODY
ITO CHAIN_BASEWORD       Read  El1=LD_CH_BASE  Exit=LD_NI_MARK
JEQ CHAIN_FANON_CK       LD_NI_MARK UNDERSCORE CHAIN_SET_FANON
ITO CHAIN_FANON_SKIP     Jump  Exit=CHAIN_RDBODY
NOLINK
ITO CHAIN_SET_FANON      Move  El1=C_1         Exit=LD_CH_FULLY_ANON
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
RVOCA CHAIN_SUB_COPY     NOITO_COPY_LINE
RVOCA CHAIN_SUB_DISP     LOAD_DISPATCH_LINE
ITO CHAIN_SUB_JMP        Jump  Exit=CHAIN_WALK_LOOP
NOLINK
/If anonymous (CH_BASE=0 or fully anon): handle accordingly
JZ CHAIN_ANON_CK LD_CH_BASE CHAIN_ANON
/Check fully anonymous mode (CHAIN _)
JEQ CHAIN_FANON_DISP LD_CH_FULLY_ANON C_1 CHAIN_FULLY_ANON
/Named: build auto-name then copy op+args
RVOCA CHAIN_NAMED_COPY   CHAIN_BUILD_NAME
RVOCA CHAIN_NAMED_DISP   LOAD_DISPATCH_LINE
ITO CHAIN_NAMED_IDX      Add   El1=LD_CH_IDX  El2=C_1          Exit=LD_CH_IDX
ITO CHAIN_NAMED_JMP      Jump  Exit=CHAIN_WALK_LOOP
/Anonymous (no name): copy line as ITO NAME op args
NOLINK
RVOCA CHAIN_ANON         NOITO_COPY_LINE_ITO
RVOCA CHAIN_ANON_DISP    LOAD_DISPATCH_LINE
ITO CHAIN_ANON_JMP       Jump  Exit=CHAIN_WALK_LOOP
/Fully anonymous (CHAIN _): emit "ITO __ch_N op args"
NOLINK
RVOCA CHAIN_FULLY_ANON   CHAIN_BUILD_ANON_NAME
RVOCA CHAIN_FA_DISP      LOAD_DISPATCH_LINE
ITO CHAIN_FA_IDX         Add   El1=LD_CH_IDX  El2=C_1          Exit=LD_CH_IDX
ITO CHAIN_FA_JMP         Jump  Exit=CHAIN_WALK_LOOP
NOLINK
RVOCA CHAIN_NEW_LUX      CHAIN_BUILD_NAME
RVOCA CHAIN_NEW_DISP     LOAD_DISPATCH_LINE
ITO CHAIN_NEW_IDX        Add   El1=LD_CH_IDX  El2=C_1          Exit=LD_CH_IDX
ITO CHAIN_NEW_JMP        Jump  Exit=CHAIN_WALK_LOOP
RREDI CHAIN_DONE
/── NOITO_BUILD_NAME: build "ITO BASE_SUFFIX_N op args" for NOITO NAME mode ───
/Identical logic to CHAIN_BUILD_NAME but uses LD_NI_BASE/LD_NI_IDX.
NEWREF NOITO_BUILD_NAME NOITO_BN_IMPL
NOLINK
ITO NOITO_BN_IMPL    Move  El1=BS_TOKBUF_BASE  Exit=LD_NI_OUT
/Write "ITO " prefix
WRITE_OUT NOITO_BN_I ASCII_I
WRITE_OUT NOITO_BN_T ASCII_T
WRITE_OUT NOITO_BN_O ASCII_O
WRITE_OUT NOITO_BN_SP SP
/Write base name from LD_NI_BASE packed string
ITO NOITO_BN_SETBASE Move  El1=LD_NI_BASE     Exit=LD_CH_BASE
RVOCA NOITO_BN_BASE  CHAIN_WRITE_PACKED
/Peek first byte: '_' → suffix, else → op
ITO NOITO_BN_PEEK    Read  El1=LD_NI_PTR      Exit=LD_TMP
JEQ NOITO_BN_USCR    LD_TMP UNDERSCORE NOITO_BN_SUFFIX
/No suffix: write '_' + N + space + rest
WRITE_OUT NOITO_BN_U UNDERSCORE
ITO NOITO_BN_SETIDX  Move  El1=LD_NI_IDX      Exit=LD_CH_IDX
RVOCA NOITO_BN_WN    CHAIN_WRITE_NUM
WRITE_OUT NOITO_BN_SP2 SP
RVOCA NOITO_BN_REST  NOITO_COPY_BODY
RREDI NOITO_BN_RRET
/Suffix: skip '_', collect until space, write '_SUFFIX_N'
NOLINK
ITO NOITO_BN_SUFFIX  Add   El1=LD_NI_PTR  El2=C_1  Exit=LD_NI_PTR
WRITE_OUT NOITO_BN_SU UNDERSCORE
ITO NOITO_BN_SFXLOOP Read  El1=LD_NI_PTR  Exit=LD_TMP
ITO NOITO_BN_SFXINC  Add   El1=LD_NI_PTR  El2=C_1  Exit=LD_NI_PTR
JEQ NOITO_BN_SFXSP   LD_TMP SP NOITO_BN_SFXDONE
JEQ NOITO_BN_SFXLF   LD_TMP LF NOITO_BN_SFXDONE
WRITE_OUT NOITO_BN_SFXW LD_TMP
ITO NOITO_BN_SFXJMP  Jump  Exit=NOITO_BN_SFXLOOP
NOLINK
ITO NOITO_BN_SFXDONE Write El1=LD_NI_OUT  El2=UNDERSCORE
ITO NOITO_BN_SFXDINC Add   El1=LD_NI_OUT  El2=C_1  Exit=LD_NI_OUT
ITO NOITO_BN_SETIDX2 Move  El1=LD_NI_IDX  Exit=LD_CH_IDX
RVOCA NOITO_BN_WN2   CHAIN_WRITE_NUM
WRITE_OUT NOITO_BN_SP3 SP
RVOCA NOITO_BN_REST2 NOITO_COPY_BODY
RREDI NOITO_BN_RRET2
/── NOITO_BUILD_ANON_NAME: build "ITO __ni_N op args" for NOITO _ mode ────────
NEWREF NOITO_BUILD_ANON_NAME NOITO_BAN_IMPL
NOLINK
ITO NOITO_BAN_IMPL   Move  El1=BS_TOKBUF_BASE  Exit=LD_NI_OUT
/Write "ITO __ni_"
WRITE_OUT NOITO_BAN_I  ASCII_I
WRITE_OUT NOITO_BAN_T  ASCII_T
WRITE_OUT NOITO_BAN_O  ASCII_O
WRITE_OUT NOITO_BAN_SP SP
WRITE_OUT NOITO_BAN_U1 UNDERSCORE
WRITE_OUT NOITO_BAN_U2 UNDERSCORE
WRITE_OUT NOITO_BAN_N  ASCII_n
WRITE_OUT NOITO_BAN_NI ASCII_i
WRITE_OUT NOITO_BAN_U3 UNDERSCORE
/Write N
ITO NOITO_BAN_SETIDX Move  El1=LD_NI_IDX  Exit=LD_CH_IDX
RVOCA NOITO_BAN_NUM  CHAIN_WRITE_NUM
/Write space + copy rest
WRITE_OUT NOITO_BAN_SP2 SP
RVOCA NOITO_BAN_REST NOITO_COPY_BODY
RREDI NOITO_BAN_RRET
/── NOITO_COPY_LINE: copy current body line bytes to BS_TOKBUF_BASE ───────────
/Reads from LD_NI_PTR until LF or end of buf. Writes to BS_TOKBUF_BASE.
/Advances LD_NI_PTR past the LF.
NEWREF NOITO_COPY_LINE NOITO_CL_IMPL
NEW LD_NI_OUT   /output write ptr into BS_TOKBUF_BASE

/── NOITO_COPY_BODY: shared inner loop — copy LD_NI_PTR→LD_NI_OUT until LF/end ──
/Leaf. Caller sets LD_NI_OUT. Writes NUL at end.
NEWREF NOITO_COPY_BODY NOITO_CB_LOOP
NOLINK
ITO NOITO_CB_LOOP        Equal El1=LD_NI_PTR  El2=LD_BODY_PTR  Exit=LD_FLAG
ITO NOITO_CB_LCKJ        JumpIf El1=LD_FLAG  Exit=NOITO_CB_DONE
READ_BODY NOITO_CB_RD LD_TMP
JEQ NOITO_CB_LFCK        LD_TMP LF NOITO_CB_DONE
WRITE_OUT NOITO_CB_WR LD_TMP
ITO NOITO_CB_JMP         Jump  Exit=NOITO_CB_LOOP
NOLINK
ITO NOITO_CB_DONE        Write El1=LD_NI_OUT  El2=C_0
RREDI NOITO_CB_RET
/── NOITO_COPY_LINE: copy current body line to BS_TOKBUF_BASE ─────────────────
NOLINK
ITO NOITO_CL_IMPL        Move  El1=BS_TOKBUF_BASE  Exit=LD_NI_OUT
    RVOCA NOITO_CL_BODY  NOITO_COPY_BODY
RREDI NOITO_CL_RRET
/── NOITO_COPY_LINE_ITO: like NOITO_COPY_LINE but prepends "ITO " ─────────────
NOLINK
ITO NOITO_CLI_IMPL       Move  El1=BS_TOKBUF_BASE  Exit=LD_NI_OUT
/Write "ITO " prefix then copy line body
WRITE_OUT NOITO_CLI_I ASCII_I
WRITE_OUT NOITO_CLI_T ASCII_T
WRITE_OUT NOITO_CLI_O ASCII_O
WRITE_OUT NOITO_CLI_SP SP
    RVOCA NOITO_CLI_BODY NOITO_COPY_BODY
RREDI NOITO_CLI_RRET
/── CHAIN_BUILD_NAME: build "ITO BASE_SUFFIX_N op args" in tokbuf ─────────────
/Reads current line from body buf (LD_NI_PTR).
/First token of line: if starts with '_' → suffix to add before _N.
/                     else              → op (no suffix, just _N).
/Builds: "ITO BASE_SUFFIX_N op args" or "ITO BASE_N op args".
/Advances LD_NI_PTR past the line.
NEWREF CHAIN_BUILD_NAME CHAIN_BN_IMPL
NEW LD_BN_TOK1_BUF    /first token bytes (for _ check)
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
RVOCA CHAIN_BN_COPY_REST NOITO_COPY_BODY
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
WRITE_OUT CHAIN_BAN_C ASCII_c
WRITE_OUT CHAIN_BAN_H ASCII_h
WRITE_OUT CHAIN_BAN_U3 UNDERSCORE
/Write N (decimal index)
RVOCA CHAIN_BAN_NUM      CHAIN_WRITE_NUM
/Write space + copy rest of line
WRITE_OUT CHAIN_BAN_SP2 SP
RVOCA CHAIN_BAN_REST     NOITO_COPY_BODY
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
ITO CHAIN_WP_NEXTLUX     Add   El1=LD_WP_LUX  El2=C_5           Exit=LD_TMP
ITO CHAIN_WP_NXTRD       Read  El1=LD_TMP      Exit=LD_WP_LUX
JZ CHAIN_WP_NXTCK        LD_WP_LUX CHAIN_WP_DONE
ITO CHAIN_WP_NXTJMP      Jump  Exit=CHAIN_WP_RDLUX
NOLINK
RREDI CHAIN_WP_DONE