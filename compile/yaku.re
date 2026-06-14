============================================================
//yaku.re: BFS + rule dispatch + LLVM IR emit
//
//NOTE: RCALL_AT calls in this file are builder-class macros — they are
//wired by LOAD_MAIN (see BUGS.md Pattern N). Until self-hosting is reached,
//the compiler entry P0_NID produces 0 bytes of output.
============================================================

── COMPILER RELATIONS ────────────────────────────────────────
ForType:    Yaku_X --ForType--> AspectLux  (maps op → rule)
Terminates: Yaku_X --Terminates--> Yaku    (marks terminator rules)

── ALU RULES ────────────────────────────────────────────────
YAKU_NEXO_ARITH Add
    {result} = add i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_ARITH Sub
    {result} = sub i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_ARITH Mul
    {result} = mul i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_ARITH Div
    {result} = sdiv i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_ARITH Rem
    {result} = srem i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_ARITH And
    {result} = and i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_ARITH Or
    {result} = or i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_ARITH Xor
    {result} = xor i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_ARITH Not
    {result} = xor i{XLEN} {v1}, -1
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_ARITH Left
    {result} = shl i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_ARITH Right
    {result} = lshr i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_ARITH ARight
    {result} = ashr i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_ARITH UDiv
    {result} = udiv i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_ARITH URem
    {result} = urem i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}

── COMPARISON RULES ─────────────────────────────────────────
YAKU_NEXO_CMP Equal
    {fresh} = icmp eq i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_CMP Less
    {fresh} = icmp slt i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_CMP ULess
    {fresh} = icmp ult i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_CMP Greater
    {fresh} = icmp slt i{XLEN} {v2}, {v1}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_CMP UGreater
    {fresh} = icmp ugt i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_CMP NotEqual
    {fresh} = icmp ne i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_CMP LessOrEqual
    {fresh} = icmp sle i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_CMP ULessOrEqual
    {fresh} = icmp ule i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_CMP GreaterOrEqual
    {fresh} = icmp sge i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU_NEXO_CMP UGreaterOrEqual
    {fresh} = icmp uge i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}

── CONTROL FLOW RULES (terminators) ─────────────────────────
YAKU_NEXO_TERM End
    ret i{XLEN} 0
/LATENT ISSUE (see runtime/regs.re for the RCALL_AT counterpart): the
/interpreter's Redi/Voca also pop/push RA_LINK on the automatic call
/stack (RA_SP), but these LLVM-IR templates don't yet emit that —
/{RA_SP_ID}/{RA_FRAME_SIZE_ID} placeholder escapes don't exist in the
/current substitution system. Compiled output would diverge from the
/interpreter for any nested call. Not yet an active bug: this whole
/compiler path is unwired (0 bytes output in both old and new builds).
/Needs: add RA_SP_ID/RA_FRAME_SIZE_ID escapes, then update both
/templates below to pop-before-jump (Redi) and push-before-jump (Voca).
YAKU_NEXO_TERM Redi
    {fresh} = load i{XLEN}, ptr getelementptr inbounds ([{TOTAL_SIZE} x i{XLEN}], ptr @heap, i{XLEN} 0, i{XLEN} {RA_LINK_ID})
    store i{XLEN} {fresh}, ptr @jr_slot
    br label %L_jrdispatch
YAKU_NEXO_TERM Jump
    br label {label_dest}
YAKU_NEXO_TERM JumpIf
    {fresh} = icmp ne i{XLEN} {v1}, 0
    br i1 {fresh}, label {label_dest}, label {label_next}

//JumpIfCmp: used when E1==RA_JEQ_FLAG and a pending i1 result is in RA_SSA_CMP.
Uses {cmp} (escape 0x0A) = pending icmp i1 SSA index directly — no load, no icmp ne.
This fuses Equal/Less/ULess/Greater + JumpIf into a single icmp + br.
System: any op with HasCmpResult lumen auto-fuses with following JumpIf via this rule.//
NEW JumpIfCmp
NEWREF JumpIfCmp JumpIfCmp
YAKU_NEXO_TERM JumpIfCmp
    br i1 %v{cmp}, label {label_dest}, label {label_next}

//JumpIfNZ: used when E1 ptr_tgt matches last arithmetic op (HasArithResult).
Uses {arith} (escape C_13) = i64 SSA result from Add/Sub/Mul/etc.
Fuses arithmetic op + JumpIf into icmp ne + br without store+load.//
NEW JumpIfNZ
NEWREF JumpIfNZ JumpIfNZ
YAKU_NEXO_TERM JumpIfNZ
    {fresh} = icmp ne i{XLEN} %v{arith}, 0
    br i1 {fresh}, label {label_dest}, label {label_next}

/I.3: Yaku_JumpReg — indirect jump, does NOT set RA_LINK (pure goto).
YAKU_NEXO_TERM JumpReg
    store i{XLEN} {v1}, ptr @jr_slot
    br label %L_jrdispatch
//Yaku_Call — sets RA_LINK = NXT_N (return address) then jumps.
NEWREF ForType
NEWREF Terminates
/HasCmpResult: marks ops that produce an i1 icmp result in {fresh}/RA_SSA_FR.
/EMIT_BLOCK saves RA_SSA_FR → RA_SSA_CMP after emitting these ops.
/Any op with this lumen auto-fuses with the following JumpIf (via JumpIfCmp).
NEWREF HasCmpResult
/HasArithResult: marks ops that produce an i64 arithmetic result in {result}/RA_SSA_RESULT.
/EMIT_BLOCK saves RA_SSA_RESULT → RA_SSA_ARITH_IDX and ptr_tgt → RA_SSA_ARITH_TGT.
/JumpIf with E1==RA_SSA_ARITH_TGT fuses into icmp ne {arith}, 0 + br (JumpIfNZ).
NEWREF HasArithResult
/Matches interpreter _voca's RA_LINK assignment (the RA_SP push is NOT
/yet emitted here — see the LATENT ISSUE note on the Redi template above).
YAKU_NEXO_TERM Voca
    store i{XLEN} {lnext_id}, ptr getelementptr inbounds ([{TOTAL_SIZE} x i{XLEN}], ptr @heap, i{XLEN} 0, i{XLEN} {RA_LINK_ID})
    store i{XLEN} {v1}, ptr @jr_slot
    br label %L_jrdispatch

── AETHER / REGISTER RULES ───────────────────────────────────
YAKU_NEXO Move store i{XLEN} {v1}, ptr {ptr_tgt}

//I.1: Yaku_Read handles Read + Load — both map to same IR.
Uses DEF_RULE_ALIAS so both ops map to same rule. Template uses {fresh} for GEP.
YAKU_NEXO_ALIAS Read
    Load {fresh} = getelementptr [{TOTAL_SIZE} x i{XLEN}], ptr @heap, i{XLEN} 0, i{XLEN} {v1}
    {result} = load i{XLEN}, ptr {fresh}
    store i{XLEN} {result}, ptr {ptr_tgt}

/I.2: Yaku_Write handles Write + Store — no target write needed.
YAKU_NEXO_ALIAS Write
    Store {fresh} = getelementptr [{TOTAL_SIZE} x i{XLEN}], ptr @heap, i{XLEN} 0, i{XLEN} {v1}
    store i{XLEN} {v2}, ptr {fresh}

── SYSTEM INTERFACE RULES ────────────────────────────────────
YAKU_NEXO Exire call i{XLEN} @_sys(i{XLEN} {v_x0}, i{XLEN} {v_x1}, i{XLEN} {v_x2}, i{XLEN} {v_x3}, i{XLEN} {v_x8})

============================================================
============================================================

── SC_*_REF: stable Lux-ID pointers for syscall code generation ─
//Problem: SC_A0..SC_NR serve dual roles:
(1) Runtime registers: compiler writes fd/buf/count into them for PB_FLUSH.
After write: SC_A0.word = 1 (stdout fd), not SC_A0_id anymore.
(2) Code-gen markers: EMIT_PRELOADS/ETH_VX* need the Lux *ID* of SC_Ax
to call PRELOAD_ARG and emit "load heap[SC_Ax_id]" in generated IR.
Solution: SC_*_REF are read-only SETREF pointers whose word = SC_*_id always.
EPL_SYS_*: Move El1=SC_A0_REF → RA_TMP  gives RA_TMP = SC_A0_id (stable).
ETH_VX*:   Move El1=SC_A0_REF → RA_SSA_LID  for GET_SSA lookup.
Both sides use the same stable ID, so PRELOAD_ARG and ETH_VX* agree on SSA.
NEWREF SC_A0_REF SC_A0
NEWREF SC_A1_REF SC_A1
NEWREF SC_A2_REF SC_A2
NEWREF SC_A3_REF SC_A3
NEWREF SC_NR_REF SC_NR

NEW P0_PRAEMIT
NEW P0_HD_DECL
NEW P0_GEPGEPS
NEW P0_GEPGM
NEW P0_GEPGMS
NEW P0_GEPGTS
NEW P0_GEPGTSI
NEW P0_GEPGTSJ
NEW P0_GEPNID
NEW P0_GEPNIDI
NEW P0_GEPNIDJ
NEW P0_GEPGEP
NEW P0_GEPGEPM
NEW P0_GEPGEPJ
NEW P0_GEPGEPMJ
NEW EB_LABEL_DONE

NEW ET_LOOP

NEW EMIT_SSA


── Registers ─────────────────────────────────────────────────
NEW RA_PA_LID
NEW RA_ETH_TW_SAVED        /ETH_* handlers: RA_TW_LUX saved before EMIT/EMITI clobbers it
NEW RA_ET_WORD             /current u64 word being scanned in packed template
NEW RA_ET_BYTE             /current extracted byte (0-255)
NEW RA_ET_SHIFT            /current bit shift within word (0,8,16,...,56)

NEW RA_NEXT_ID       /live value of K_CURSOR (current alloc_ptr = next free address)
NEW RA_ENTRY_LUX     /current compiler entry Lux
NEW RA_INSTR         /current instruction Lux being compiled
NEW RA_OP_ID         /Op primitive ID of current instruction
NEW RA_RULE          /Yaku_X Lux for current primitive



NEW RA_SSA_CTR       /monotonically increasing, 0 at function start

NEW RA_SSA_V1_DATA                         /data slot for RA_SSA_V1 value
NEWREF RA_SSA_V1 RA_SSA_V1_DATA            /pointer: word = ID of DATA
NEW RA_SSA_V2_DATA                         /data slot for RA_SSA_V2 value
NEWREF RA_SSA_V2 RA_SSA_V2_DATA            /pointer: word = ID of DATA
NEW RA_SSA_OUT                             /SSA index for result register (output of PRELOAD_ARG)
NEW RA_SSA_RESULT_DATA                     /data slot for RA_SSA_RESULT value
NEWREF RA_SSA_RESULT RA_SSA_RESULT_DATA    /pointer: word = ID of DATA
NEW RA_SSA_FR_DATA                         /data slot for {fresh} SSA value
NEWREF RA_SSA_FR RA_SSA_FR_DATA            /pointer: word = ID of DATA
NEW RA_SSA_FR2_DATA                        /data slot for {fresh2} SSA value
NEWREF RA_SSA_FR2 RA_SSA_FR2_DATA          /pointer to FR2 data

NEW RA_A1            /El1 lux_id
NEW RA_A2            /El2 lux_id
NEW RA_EXIT_N        /Exit lux_id (SLOT_EXIT value)
NEW RA_NXT_N         /Next lux_id

NEW RA_LIMIT         /general loop limit

NEW RA_EM_I          /EMIT_MISSED outer loop counter (current Lux ID being examined)

/EB_OPS_BODY uses JEQ chain on rel addresses (no runtime table needed)

NEW RA_JR_BI         /inner bit-loop counter (0..63) for JR dispatch scan
NEW RA_JR_WORD       /current VS bitset word during JR dispatch scan
NEW RA_JR_LID        /computed Lux ID = word_idx*64 + bit_idx

NEW RA_RT_BASE       /base address of rule-lookup table (op_id → Yaku_X address)
NEW RA_VS_WORDS      /number of words in visited bitset = (alloc_ptr >> 6) + 1 (dynamic)
NEW RA_SSA_BASE      /base of SSA direct-map array (RA_NEXT_ID luces, indexed by Lux address)
NEW RA_SSA_GEN       /generation counter — incremented instead of clearing the SSA array
NEW RA_SSA_LID       /IN param for GET_SSA: Lux ID to look up (replaces RA_HT_HASH misuse)
NEW RA_SSA_HIT       /OUT param for GET_SSA: nonzero if slot was hit, 0 if new
/RA_SSA_CMP: SSA index of last i1 icmp result from Equal/Less/ULess.
/Persists across EMIT_BLOCK calls (NOT cleared per-BB) so JumpIf can use it directly.
/0 = no pending comparison (cleared when any non-comparison ITO is processed).
NEW RA_SSA_CMP
/RA_SSA_ARITH_IDX: SSA index of last arithmetic op result (Add/Sub/etc.) — i64 type.
/RA_SSA_ARITH_TGT: ptr_tgt addr that the arithmetic op wrote result to.
/When JumpIf.E1 == RA_SSA_ARITH_TGT: fuse into icmp ne {result}, 0 + br (JumpIfNZ).
NEW RA_SSA_ARITH_IDX
NEW RA_SSA_ARITH_TGT
/RA_PC_BASE and RA_BEN_PC_ADDR removed — predecessor-count array eliminated.

── Subroutine registers ──────────────────────────────────────
NEW RA_FAL_REG       /ET_PH_FRESH_ALLOC: address of SSA register to check/assign
NEW RA_ESAR_REG      /ET_EMIT_SSA_REG:   address of SSA register whose value to emit as %vN

── Template escape bytes (see loader.py _TMPL_ESCAPE for mapping) ──
//Packed string format used by EMIT_TMPL: escape bytes 0x01-0x13 for placeholders.
Parser.re still builds RO_* graph luces at runtime for dynamic NEXO templates
(parser-built rules, not loader-built). RO_* definitions kept for compatibility.


18 JEQ instructions, zero runtime initialization cost.

SC_ARGS table removed — EPL_SYS preload uses direct SC_A0..SC_NR references

── Constants ─────────────────────────────────────────────────

NC_* removed — Rule templates are now stored as RO_* graphs, not strings.
ET_DISPATCH and string-based placeholder dispatch are gone.
See RO_* declarations above.

/SF_X_I64 value set by compiler_sf.re (generated by gen_compiler.py)
/SF_FUNCOPEN value set by compiler_sf.re (generated by gen_compiler.py)
/Secondary Entry: no @_init; name = @L<lux_id>

//SF_GEP_MID value set by compiler_sf.re (generated by gen_compiler.py)
NEWREF RO_LITERAL   /runtime: emit literal string (El1 = byte-chain Lux)
NEWREF RO_NEWLINE   /runtime: emit LF
NEWREF RO_V1        /runtime: emit %v<SSA[El1]>
NEWREF RO_V2        /runtime: emit %v<SSA[El2]>
NEWREF RO_RESULT    /runtime: emit %v<fresh alloc-or-reuse>
NEWREF RO_FRESH     /runtime: emit %v<fresh alloc, SSA_FR>
NEWREF RO_FRESH2    /runtime: emit %v<fresh alloc, SSA_FR2>
NEWREF RO_PTR_TGT   /runtime: emit %pt<SSA_PT>
NEWREF RO_LDEST     /runtime: emit %L<EXIT_N>
NEWREF RO_LNEXT     /runtime: emit %L<NXT_N>
NEWREF RO_LNEXT_ID  /runtime: emit raw integer NXT_N
NEWREF RO_XLEN      /runtime: emit XLEN constant
NEWREF RO_TOTALSZ   /runtime: emit TOTAL_SIZE constant
NEWREF RO_FLB       /runtime: emit FIRST_LUMEN_BASE constant
NEWREF RO_VX0       /runtime: emit %v<SSA[SC_A0]>
NEWREF RO_VX1       /runtime: emit %v<SSA[SC_A1]>
NEWREF RO_VX2       /runtime: emit %v<SSA[SC_A2]>
NEWREF RO_VX3       /runtime: emit %v<SSA[SC_A3]>
NEWREF RO_VX8       /runtime: emit %v<SSA[SC_NR]>
NEWSET SF_VPX "%v"
NEWSET SF_LBL "%L"
NEWSET SF_INDENT "  "
NEWSET SF_BR "  br label %L"
NEWSET SF_HEAPDECL "@heap = global ["
NEW SF_X_I64
NEW SF_FUNCOPEN
NEWSET SF_FUNCMAIN "main() {\nentry:\n  call void @reca_init()\n  br label %L"
NEWSET SF_FUNCEND "}\n\n"
NEWSET SF_FUNCSEC "reca_"
NEWSET SF_FUNCSECBODY "() {\nentry:\n  br label %L"
NEW RA_EP_COUNT   /counts Entry→Yaku found; 0=first(@main), >0=secondary(@L<id>)
NEW SF_GEP_MID
── Inline GEP strings (SSA optimisation: no separate %ptN register) ─
Load: "  %vN = load i64, ptr getelementptr inbounds ([S x i64], ptr @heap, i64 0, i64 id)\n"
NEWSET SF_LOAD_IGEP_PRE " = load i64, ptr getelementptr inbounds (["
/Ptr for store template: "ptr getelementptr inbounds ([S x i64], ptr @heap, i64 0, i64 id)"
NEWSET SF_PTR_IGEP "getelementptr inbounds (["
/Close with newline (for load/store lines) or without (for mid-line ptr_tgt)
NEWSET SF_IGEP_CLOSE_NL ")\n"
NEWSET SF_IGEP_CLOSE ")"
NEWSET SF_DECLARE "@jr_slot = global i64 0\ndeclare i64 @write(i32, ptr, i64)\ndeclare i64 @read(i32, ptr, i64)\ndeclare i32 @openat(i32, ptr, i32, i32)\ndeclare i32 @close(i32)\ndeclare void @exit(i32)\n"

NEWSET SF_UNREACHABLE "  unreachable\n"

── JumpReg dispatch emission ─────────────────────────────────
NEWSET SF_JR_HEAD "\nL_jrdispatch:\n"
NEW SF_JR_LOAD
/SF_JR_LOAD value set by compiler_sf.re (generated by gen_compiler.py)
NEW SF_JR_ARM_PFX
/SF_JR_ARM_PFX value set by compiler_sf.re (generated by gen_compiler.py)
NEWSET SF_JR_ARM_MID ", label %L"
NEW SF_JR_TAIL
//SF_JR_TAIL value set by compiler_sf.re (generated by gen_compiler.py)


── INIT_ARRAYS: allocate BFS queue, visited bitset, SSA direct-map array ─
Non-leaf — RA_LINK is saved/restored automatically by the call stack.

SSA table: direct-map array of K_AETHER_SIZE words, indexed by Lux ID.
Slot format: (generation << 32) | ssa_index.
"Clear" = increment RA_SSA_GEN (O(1)). Lookup: read slot, check generation.
No hash, no probe, no collision possible. O(1) guaranteed.

ALLOC_RAW BFS_QUEUE_SIZE RA_BQ_BASE

//VS words = (RA_NEXT_ID >> 6) + 1: dynamic, covers all allocated addresses.
Same pattern as INIT_SSA_A — size from actual alloc_ptr, not hardcoded constant.
ITO INIT_VS_SHR  Right     El1=RA_NEXT_ID El2=C_6  Exit=RA_VS_WORDS
ITO INIT_VS_INC  Add     El1=RA_VS_WORDS El2=C_1  Exit=RA_VS_WORDS
ALLOC_RAW RA_VS_WORDS RA_VS_BASE

//SSA direct-map: RA_NEXT_ID slots (one per allocated address).
RA_NEXT_ID = alloc_ptr at P0_NID start — exact size, no overallocation.
Generation 0 = empty slot. O(1) lookup guaranteed.
ALLOC_RAW RA_NEXT_ID RA_SSA_BASE
ITO INIT_SSA_GEN  Move El1=C_1 Exit=RA_SSA_GEN

//Predecessor-count array removed — was used in BFS_ENQ but never consumed elsewhere.

── INIT_RT: allocate rule-lookup table (C_1024 luces) ─
Indexed by aspect Lux address. Max derived aspect addr ~628 (Store).
C_1024 covers all current and future aspects with ample headroom.
ALLOC_RAW C_1024 RA_RT_BASE

//INIT_SC_ARGS removed — no runtime table needed

INIT_RO_TABLE removed — dispatch via JEQ chain in ET_LOAD (no runtime init needed)

── INIT_OPS_JT removed: EB_OPS_BODY now uses JEQ chain on rel address ──
No runtime table needed. Rel addresses are stable (8-slot Lux, SETREF).
Dispatch: compare RA_TMP (rel address) with El1/El2/AutoNext.
RREDI INIT_REGS

── VS_TEST_SET: atomic test-and-set on the visited bitset ─────
//IN:  RA_VS_ID  = Lux ID to test (caller must set before jumping)
RA_LINK    = return address
OUT: RA_FLAG   = 0 if Lux was NOT visited (bit just set for first time)
nonzero if Lux was ALREADY visited (bit was already set)
The bit is ALWAYS set after this call.
Clobbers: RA_TMP2 (lux addr), RA_TMP3 (bit mask), RA_TMP4 (word value)

── VS_PREPARE: compute word addr + bit mask for RA_VS_ID ──────
/OUT: RA_TMP2 = lux addr in VS array, RA_TMP3 = bit mask. Leaf.
NOLINK
ITO VS_PREPARE   Right   El1=RA_VS_ID    El2=C_6      Exit=RA_TMP2  /word_idx = id >> 6
ITO VSP_BIT      And     El1=RA_VS_ID    El2=C_63     Exit=RA_TMP3  /bit_pos  = id & 63
ITO VSP_MASK     Left    El1=C_1         El2=RA_TMP3  Exit=RA_TMP3  /bit_mask
ITO VSP_ADDR     Add     El1=RA_VS_BASE  El2=RA_TMP2  Exit=RA_TMP2  /lux addr
RREDI VSP_RET

NOLINK
RVOCA VS_TEST_SET VS_PREPARE
ITO VTS_LOAD     Read El1=RA_TMP2                   Exit=RA_TMP4  /word value
ITO VTS_TEST     And     El1=RA_TMP4     El2=RA_TMP3  Exit=RA_FLAG  /test bit
ITO VTS_SET      Or      El1=RA_TMP4     El2=RA_TMP3  Exit=RA_TMP4  /set bit
ITO VTS_STORE    Write El1=RA_TMP2    El2=RA_TMP4                  /write back
RREDI VTS_RET

── VS_CLEAR_BIT: clear one bit in the visited bitset ─────────
//IN:  RA_VS_ID = Lux ID to clear. RA_LINK = return address.
OUT: (none). Clobbers: RA_TMP2 (lux addr), RA_TMP3 (bit mask), RA_TMP4 (word).
Symmetric to VS_TEST_SET. Leaf.
NOLINK
RVOCA VS_CLEAR_BIT VS_PREPARE
ITO VCB_INV      Xor     El1=RA_TMP3     El2=C_NEG1   Exit=RA_TMP3  /inverted mask
ITO VCB_LOAD     Read El1=RA_TMP2                   Exit=RA_TMP4  /word value
ITO VCB_CLR      And     El1=RA_TMP4     El2=RA_TMP3  Exit=RA_TMP4  /clear bit
ITO VCB_STORE    Write El1=RA_TMP2    El2=RA_TMP4                  /write back
RREDI VCB_RET

── BQ_ADDR — compute circular queue lux address ─────────────
/IN:  RA_TMP = raw pointer. OUT: RA_TMP = lux address. Leaf.
NOLINK
ITO BQ_ADDR     Rem  El1=RA_TMP El2=BFS_QUEUE_SIZE   Exit=RA_TMP
ITO BQ_ADDR_ADD Add  El1=RA_BQ_BASE El2=RA_TMP Exit=RA_TMP
RREDI BQ_ADDR_RET

── VS_CLEAR — zero the visited bitset (RA_VS_WORDS luces) ─────────────────
/Leaf. Clobbers: RA_I, RA_TMP
NOLINK
CLEAR VS_CLEAR RA_I
JEQ   VS_CLR_LOOP  RA_I   RA_VS_WORDS     VS_CLR_DONE
ITO VS_CLR_ST    Add     El1=RA_VS_BASE El2=RA_I  Exit=RA_TMP
ITO VS_CLR_WR    Write El1=RA_TMP    El2=C_0
ITO VS_CLR_INC   Add     El1=RA_I       El2=C_1   Exit=RA_I
ITO VS_CLR_LB    Jump    Exit=VS_CLR_LOOP
RREDI VS_CLR_DONE

//ET_PACK_BYTE removed — byte packing is now inline in ET_COLL_W0/W1
(pointer-through-register pattern was buggy: Read of a value, not an ID)

── ET_PH_FRESH_ALLOC — alloc-or-reuse SSA index ──────────────
IN:  RA_FAL_REG = Lux ID of SSA register to check/assign
Non-leaf (calls EMIT_INT_ENTRY); RA_LINK saved/restored automatically.
Also saves RA_TW_LUX via SAVE macro — EMIT_INT_ENTRY/EMIT_PACKED_STR
clobber RA_TW_LUX (used as walker). SAVE restores automatically before return.
NOLINK
SAVE ET_PH_FRESH_ALLOC RA_TW_LUX
    ITO FAL_RDVAL           Read El1=RA_FAL_REG   Exit=RA_TMP4
    JZ    FAL_CHECK           RA_TMP4 FAL_NEW
    /Already assigned: emit %v + existing value
    EMIT  FAL_REUSE_VPX       SF_VPX
    ITO FAL_REUSE_INT  Move  El1=RA_TMP4             Exit=RA_TMP2
    RCALL_AT FAL_REUSE_INTR EMIT_INT_ENTRY FAL_DONE
    /Allocate new SSA index
    ITO FAL_NEW        Add   El1=RA_SSA_CTR    El2=C_1   Exit=RA_SSA_CTR
    ITO FAL_SAVEPRI    Write El1=RA_FAL_REG  El2=RA_SSA_CTR
    //Secondary register removed — it was corrupting native Lux words by using
    raw SSA index as Write address. Each SSA slot has its own pointer Lux.
    EMIT  FAL_EMIT       SF_VPX
    ITO FAL_SET_INT2   Move  El1=RA_SSA_CTR    Exit=RA_TMP2
    RVOCA FAL_EMIT_INT EMIT_INT_ENTRY
RREDI FAL_DONE
── ET_EMIT_SSA_REG — emit "%vN" for a given SSA register ──────
//IN:  RA_ESAR_REG = Lux ID of the SSA index register
Non-leaf. Saves RA_TW_LUX via SAVE macro.
NOLINK
SAVE ET_EMIT_SSA_REG RA_TW_LUX
    ITO ESAR_RDVAL        Read El1=RA_ESAR_REG  Exit=RA_TMP2
    EMIT  ESAR_VPX           SF_VPX
    RVOCA ESAR_SETRET EMIT_INT_ENTRY
RREDI ESAR_DONE_r

/EMIT_GEP_LINE removed - superseded by inline GEP

── P0_NID: ENTRY POINT ───────────────────────────────────────
ITO P0_NID      Read El1=K_CURSOR  Exit=RA_NEXT_ID
RCALL_AT P0_INIT_C INIT_BFS RT_BUILD_ENTRY

── RT_BUILD: populate rule-lookup table ─────────────────────
//After INIT returns, scan all Lux for ForType Lumen.
For each Lux with a ForType lumen → op_id, write RT_BASE[op_id] = lux_id.
One Rule may have multiple ForType Lumen (e.g. Yaku_Read for Load+Read),
so all are registered here.
O(total_lumens) once at startup.
Use SCAN_ALL_LUX to iterate all Data Lux via LUX_REGISTRY.
Body: RTB_BODY — for each Lux, scan its ForType lumens.
NOLINK
ITO RT_BUILD_ENTRY Move El1=RTB_BODY Exit=RA_SCAN_BODY
RVOCA RTB_SCAN_J    SCAN_ALL_LUX
ITO RTB_DONE   Jump Exit=P0_ALLOC_EPBUF

── RTB_BODY: per-Lux body called by SCAN_ALL_LUX ─────────────
//IN: RA_I = current Lux address. Scans its Lumen for ForType lumens.
Non-leaf: saves RA_LINK, RA_SCAN_STOP, RA_SCAN_BODY (inner SLO overwrites).
RA_SCAN_BODY MUST be saved/restored: SCAN_LUMEN_OF sets it to RTB_LUX_BODY,
but outer SCAN_ALL_LUX needs it restored to RTB_BODY for next iteration.
NOLINK
SAVE RTB_BODY RA_SCAN_STOP RA_SCAN_BODY
    ITO RTB_SET_LUX   Move El1=RA_I          Exit=RA_SCAN_LUX
    ITO RTB_SLO_BD    Move El1=RTB_LUX_BODY  Exit=RA_SCAN_BODY
    RVOCA RTB_SLO_J     SCAN_LUMEN_OF

RREDI RTB_BODY_RET

── RTB_LUX_BODY: per-Lumen body for RT_BUILD ─────────────────
//IN: RA_TW_LUMEN = current Lumen. Non-leaf (LR/LT expand to Voca).
If rel==ForType: read exit (op_id), bounds check, write RT_BASE[op_id]=RA_I.
NOLINK
NORESTORE
/NOTE: SAVE includes RA_LINK here, which is provably a no-op under the
/automatic call stack (no internal Voca in this body — LR/LT expand to
/plain Move/Read at macro-expansion time, not runtime calls). Left as-is:
/touching this SAVE list risks the same kind of stack-imbalance issue as
/EM_MISSED_BODY below (S_RA_LINK is also used there as ad-hoc scratch via
/EM_VS_OUTER); not worth the risk for a provable no-op. Safe to remove
/RA_LINK from this list in a future pass if EM_MISSED_BODY is untangled too.
SAVE RTB_LUX_BODY RA_LINK
    LR RTB_LR RA_TW_LUMEN RA_TMP          /rel
    JEQ RTB_FTK RA_TMP ForType RTB_FT
    ITO RTB_BODY_SKIP Move El1=S_RA_LINK Exit=RA_LINK
    RREDI RTB_BODY_SKIP_r                   /not ForType → continue
    LT RTB_FT RA_TW_LUMEN RA_TMP          /exit = op_id
    ITO RTB_BCHK   ULess El1=RA_TMP El2=C_1024 Exit=RA_TMP2  /op_id < 1024
    ITO RTB_BCHKJ  JumpIf El1=RA_TMP2 Exit=RTB_ADDR
    ITO RTB_OOB    Move    El1=S_RA_LINK Exit=RA_LINK
    RREDI RTB_OOB_RET                       /out of range → skip
    ITO RTB_ADDR   Add      El1=RA_RT_BASE El2=RA_TMP Exit=RA_TMP2
    ITO RTB_WRITE  Write El1=RA_TMP2   El2=RA_I
    ITO RTB_LUX_RESTORE Move El1=S_RA_LINK Exit=RA_LINK
    RREDI RTB_WRITE_RET

── P0_COLLECT: scan LUX_REGISTRY, collect Entry luces ──────────────
//We do this as a PURE scan (no nested scans, no EMIT_FUNC calls).
For each lux with Entry→Yaku or Entry→SelfYaku, store addr in EP_BUFFER.
EP_BUFFER is allocated BEFORE P0_PREAM_DONE resets K_CURSOR so it lands
above BFS/VS/SSA/RT scratch and survives the reset.
Max capacity: EP_BUF_MAX entries (32 is generous — real programs have few entries).

Allocate EP_BUFFER first, while bump is still above BFS/VS/SSA scratch.
Then emit preamble and reset K_CURSOR — EP_BUF is safely above the freed region.
NEWSET EP_BUF_MAX 32
NEW RA_EP_BUF_BASE    /base address of EP_BUFFER
NEW RA_EP_BUF_CNT     /how many entries collected

ALLOC_RAW EP_BUF_MAX RA_EP_BUF_BASE
CLEAR P0_EPBUF_CLR RA_EP_BUF_CNT

EMIT P0_INIT_DONE SF_DECLARE
EMIT P0_HEAPD SF_HEAPDECL
EMITI P0_HD_NUM K_AETHER_SIZE
EMIT P0_HD_XI64 SF_X_I64
PUTBYTE P0_PREAM LF
ITO P0_PREAM_TW Move El1=RA_PREAM_BASE Exit=RA_TW_LUX
RVOCA P0_PREAM2 EMIT_STR_ENTRY
/Reset K_CURSOR to reclaim BFS/VS/SSA/RT scratch. EP_BUF is safely above.
ITO P0_PREAM_DONE Write El1=K_CURSOR El2=RA_NEXT_ID
CLEAR P0_INIT_FUNC_DONE RA_EP_COUNT

/Collect: scan LUX_REGISTRY with P0_COLLECT_BODY (no nested scans inside body)
ITO P0_SET_COL_BODY Move El1=P0_COLLECT_BODY Exit=RA_SCAN_BODY
RVOCA P0_COLSCANJ   SCAN_ALL_LUX
ITO P0_COLLECT_DONE Jump Exit=P0_EMIT_INIT   /SCAN_ALL_LUX returns here; jump to P0_EMIT
//IN: RA_I = Lux address. Check Entry→Yaku/SelfYaku via direct slot read (O(1)).
Does NOT call any other scan. Does NOT call EMIT_FUNC.
NOLINK
NORESTORE
SAVE P0_COLLECT_BODY
    /Fast Entry check: slot 7 = first extra lumen rel (for ITO luxs with compact layout)
    ITO P0_CB_OPCHK    Add  El1=RA_I El2=C_1        Exit=RA_TMP   /slot 1 = op
    ITO P0_CB_OPRD     Read El1=RA_TMP              Exit=RA_TMP
    ITO P0_CB_OPTEST   JumpIf El1=RA_TMP             Exit=P0_CB_ITO
    /Data lux: lumens at slot 1. Check (rel=Entry at pos 1)
    ITO P0_CB_DATA_REL Add  El1=RA_I El2=C_1        Exit=RA_TMP
    ITO P0_CB_DATA_RD  Read El1=RA_TMP              Exit=RA_FLAG
    JEQ P0_CB_DATA_EQ RA_FLAG Entry P0_CB_DATA_EXIT
    ITO P0_CB_SKIP     Move El1=S_RA_LINK           Exit=RA_LINK
    RREDI P0_CB_SKIP_r
    CHAIN
        P0_CB_DATA_EXIT Add  El1=RA_I El2=C_2        Exit=RA_TMP
        P0_CB_DATA_TCK  Read El1=RA_TMP              Exit=RA_TMP2
            ITO P0_CB_DATA_JMPIF JumpIf El1=RA_TMP2 Exit=P0_CB_CHK_TGT
        P0_CB_SKIP2     Move El1=S_RA_LINK           Exit=RA_LINK
            RREDI P0_CB_SKIP2_r
    /ITO lux: extra lumen at slot 7
    CHAIN
        P0_CB_ITO      Add  El1=RA_I El2=C_7        Exit=RA_TMP
        P0_CB_ITO_RD   Read El1=RA_TMP              Exit=RA_FLAG
            JEQ P0_CB_ITO_EQ RA_FLAG Entry P0_CB_ITO_EXIT
        P0_CB_NENT     Move El1=S_RA_LINK           Exit=RA_LINK
            RREDI P0_CB_NENT_r
    NOLINK
    ITO P0_CB_ITO_EXIT  Add  El1=RA_I El2=C_8        Exit=RA_TMP   /slot 8 = exit
    ITO P0_CB_ITO_RD  Read El1=RA_TMP              Exit=RA_TMP2  /exit → RA_TMP2 (not RA_FLAG!)
    /Check exit == Yaku or SelfYaku — use RA_TMP2 to preserve value across Equals
    JEQ P0_CB_CHK_TGT RA_TMP2 Yaku P0_CB_STORE
    JEQ P0_CB_CHK_CL RA_TMP2 SelfYaku P0_CB_STORE
    ITO P0_CB_NOEP     Move El1=S_RA_LINK           Exit=RA_LINK
    RREDI P0_CB_NOEP_r
    NOLINK
    /Store RA_I in EP_BUFFER
    ITO P0_CB_STORE    Add  El1=RA_EP_BUF_BASE El2=RA_EP_BUF_CNT Exit=RA_TMP
    ITO P0_CB_WRITE    Write El1=RA_TMP El2=RA_I
    ITO P0_CB_INC      Add  El1=RA_EP_BUF_CNT El2=C_1 Exit=RA_EP_BUF_CNT
    ITO P0_CB_RET      Move El1=S_RA_LINK           Exit=RA_LINK
    RREDI P0_CB_RET_r

── P0_EMIT: iterate EP_BUFFER, emit one function per Entry ──────────
//Simple index loop: RA_EP_IDX from 0 to RA_EP_BUF_CNT.
No external scan active during EMIT_FUNC — safe for nested scans.
CLEAR P0_EMIT_INIT RA_EP_COUNT
NEW RA_EP_IDX          /current index into EP_BUFFER
CLEAR P0_IDX_CLR RA_EP_IDX
JEQ P0_EMIT_LOOP RA_EP_IDX RA_EP_BUF_CNT P0_DONE
/Load entry addr from buffer
ITO P0_EMIT_ADDR  Add  El1=RA_EP_BUF_BASE El2=RA_EP_IDX Exit=RA_TMP
ITO P0_EMIT_RD    Read El1=RA_TMP                       Exit=RA_ENTRY_LUX
/Dispatch: first entry → EMIT_FUNC (primary/@main), rest → EMIT_FUNC_SECONDARY
JZ P0_EMIT_CHK RA_EP_COUNT P0_EMIT_PRIMARY
ITO P0_EMIT_SEC   Add  El1=RA_EP_COUNT El2=C_1          Exit=RA_EP_COUNT
RCALL_AT P0_EMIT_SECJ EMIT_FUNC_SECONDARY P0_EMIT_NEXT
NOLINK
ITO P0_EMIT_PRIMARY Add El1=RA_EP_COUNT El2=C_1         Exit=RA_EP_COUNT
RCALL_AT P0_EMIT_PRIMJ EMIT_FUNC P0_EMIT_NEXT
/Landing after EMIT_FUNC returns
NOLINK
ITO P0_EMIT_NEXT  Add  El1=RA_EP_IDX El2=C_1            Exit=RA_EP_IDX
ITO P0_EMIT_LB    Jump Exit=P0_EMIT_LOOP

RVOCA P0_DONE FLUSH
ITO P0_HALT     End

//P0_PREAM_TW: RA_TW_LUX must receive the ID of PREAM_0000, not its word.
Move El1=PREAM_0000 reads aether[PREAM_0000] = packed bytes (word) — wrong.
SETREF RA_PREAM_BASE PREAM_0000 at freeze sets aether[RA_PREAM_BASE]=PREAM_0000_id.
Then Read RA_PREAM_BASE → RA_TW_LUX gives the correct ID.
SETREF RA_PREAM_BASE PREAM_0000
//EMIT_STR_ENTRY in output.re already decodes packed u64 byte chains identically.
NEW RA_PREAM_BASE

── EMIT_FUNC: emit one LLVM function ────────────────────────
Two entry points:
EMIT_FUNC         → primary: "@main" with @_init call (EP_COUNT==1)
EMIT_FUNC_SECONDARY → secondary: "@L<id>" without @_init (EP_COUNT>1)
Both share EMIT_FUNC_BODY for BFS block emission.

Primary entry: define i64 @main() { entry: call @_init(); br label %L<id> }
EMIT EMIT_FUNC SF_FUNCOPEN
EMIT EF_OPENM SF_FUNCMAIN
EMITI EF_OPENML RA_ENTRY_LUX
PUTBYTE EF_OPENNL LF
ITO EF_TO_BODY Jump Exit=EF_VSCLEAR

//Secondary entry: full function — header only differs from primary (no @reca_init).
LLVM labels are function-local; secondary cannot branch into @main's labels.
Solution: secondary runs its own EMIT_FUNC_BODY (VS clear, BFS from own entry, etc.).
NOLINK
EMIT EMIT_FUNC_SECONDARY SF_FUNCOPEN
EMIT EF_SEC_L SF_FUNCSEC
EMITI EF_SEC_ID RA_ENTRY_LUX
EMIT EF_SEC_BODY SF_FUNCSECBODY
EMITI EF_SEC_BRL RA_ENTRY_LUX
PUTBYTE EF_SEC_NL LF
ITO EF_SEC_TO_BODY Jump Exit=EF_VSCLEAR

/Common body: VS clear, BFS, EMIT_MISSED, JR_DISPATCH — runs ONCE for primary only.
NOLINK
/I.13: replaced inline VS clear loop with VS_CLEAR subroutine.
RVOCA EF_VSCLEAR VS_CLEAR
CLEAR EF_VSCLEAR_DONE RA_SSA_CTR
/Invalidate SSA cache at function boundary — new generation, all slots stale.
ITO EF_SSA_GEN_INC  Add  El1=RA_SSA_GEN El2=C_1 Exit=RA_SSA_GEN
CLEAR EF_BQRST RA_BQ_HEAD
CLEAR EF_BQRST2 RA_BQ_TAIL
ITO EF_ENQUEUE Move El1=RA_ENTRY_LUX Exit=RA_TMP
RVOCA EF_ENQ_R BFS_ENQ
RVOCA EF_BFSJ BFS_LOOP_SAVE
ITO EF_BFS_DONE Move El1=EF_EMISS Exit=RA_EM_OUTER
ITO EF_BMISSJ   Jump  Exit=EMIT_MISSED
ITO EF_EMISS Move El1=EF_EJRD Exit=RA_JRD_OUTER
ITO EF_EMISSJ   Jump  Exit=EMIT_JR_DISPATCH
EMIT EF_EJRD SF_FUNCEND
ITO EF_RESTORE_SCAN Move El1=RA_ENTRY_LUX Exit=RA_SCAN_LUX  /restore RA_SCAN_LUX clobbered by RTB/BFS_SCAN_LUX inside EMIT_FUNC
RREDI EF_RET_r

── EMIT_MISSED: emit basic blocks not reached by BFS ──────────
//Uses SCAN_ITO_LUX (instruction-only registry) so RO/Data Lux
are not accidentally compiled as instruction blocks.////
ITO EMIT_MISSED  Move El1=EM_MISSED_BODY Exit=RA_SCAN_BODY
RVOCA EM_SCANJ SCAN_ITO_LUX
RREDI EM_DONE_r

── EM_MISSED_BODY: per-Lux body for EMIT_MISSED ───────────────
//IN: RA_I = Lux address. Non-leaf (WALK_ONE + VS_TEST_SET + RCALL_AT).
Saves RA_LINK, RA_SCAN_BODY, RA_SCAN_STOP so EMIT_BLOCK doesn't clobber them.//
/NOTE on RA_LINK: under the automatic call stack, RA_LINK save/restore here
/would normally be redundant too — EXCEPT S_RA_LINK is ALSO reused below
/(EM_HASOP/EM_VTS_DONE) as ad-hoc scratch storage across the VS_TEST_SET
/call, unrelated to its "saved return address" role. Untangling that reuse
/requires understanding intent in code whose output is currently untestable
/(EMIT_TMPL/yaku.re produce 0 bytes in both old and new builds). Left as-is
/pending a dedicated pass once the compiler's output can be verified.
NOLINK
NORESTORE
SAVE EM_MISSED_BODY RA_LINK RA_SCAN_BODY RA_SCAN_STOP RA_SAL_CUR SAL_REG_END
    ITO EM_SET_I        Move El1=RA_I          Exit=RA_EM_I
    /Check Op: read directly from SLOT_OP (compact ITO slot 1)
    ITO EM_OPCHK     Add     El1=RA_EM_I   El2=C_1   Exit=RA_TMP
    ITO EM_OPDRD     Read El1=RA_TMP                 Exit=RA_FLAG
    ITO EM_OPSRDJ    JumpIf  El1=RA_FLAG             Exit=EM_EPRD7
    ITO EM_OPNOOP    Jump    Exit=EM_NOOP
    NOLINK
    //Has Op: fast O(1) Entry check — read slot 7 (first extra lumen rel).
    ITO luxs with LINK X Entry Yaku store (Entry_addr, Loom_addr) at slots 7,8.
    SelfYaku entries also have Entry at slot 7 — now also skipped (separate function).//
    NOLINK
    ITO EM_EPRD7     Add     El1=RA_EM_I   El2=C_7   Exit=RA_TMP    /slot 7 = first extra lumen rel
    ITO EM_EPRDREL   Read El1=RA_TMP                Exit=RA_FLAG   /read rel
    JEQ EM_EPRELEQ RA_FLAG Entry EM_EPTGT
    ITO EM_EPNO_ENT  Jump   Exit=EM_HASOP                          /no Entry lumen → compile
    NOLINK
    /Has Entry lumen: read exit at slot 8
    ITO EM_EPEXIT    Add    El1=RA_EM_I   El2=C_8   Exit=RA_TMP    /slot 8 = exit
    ITO EM_EPRDEXIT  Read El1=RA_TMP               Exit=RA_FLAG   /read exit
    JEQ EM_EPLM_CK RA_FLAG Yaku EM_NOOP
    JEQ EM_EPCLM_CK RA_FLAG SelfYaku EM_NOOP
    /Other Entry target → fall through to compile
    ITO EM_EPNO_LOOM Jump   Exit=EM_HASOP
    NEW EM_VS_OUTER
    ITO EM_HASOP     Move   El1=S_RA_LINK Exit=EM_VS_OUTER
    ITO EM_SETVSID   Move   El1=RA_EM_I  Exit=RA_VS_ID
    RVOCA EM_VTSR VS_TEST_SET
    ITO EM_VTS_DONE  Move   El1=EM_VS_OUTER Exit=S_RA_LINK
    ITO EM_VTS_CHK   JumpIf El1=RA_FLAG   Exit=EM_VISITED
    ITO EM_EMIT      Move   El1=RA_EM_I   Exit=RA_INSTR
    //Set RA_EB_OUTER=EM_VISITED: EMIT_BLOCK returns here, not to BFS_SCAN_LUX.
    EB_SAVE_RET was wrong: it set RA_EB_OUTER=BFS_SCAN_LUX causing BFS restart per block.
    ITO EM_EB_OUTER  Move   El1=EM_VISITED Exit=RA_EB_OUTER
    ITO EM_EMITJ     Jump   Exit=EMIT_BLOCK
    NOLINK
    /Shared restore path for NOOP and VISITED — identical register restores
    ITO EM_NOOP      Move El1=S_RA_SCAN_BODY Exit=RA_SCAN_BODY
    ITO EM_VISITED   Move El1=S_RA_SCAN_STOP Exit=RA_SCAN_STOP
    ITO EM_RST_RSC   Move El1=S_RA_SAL_CUR   Exit=RA_SAL_CUR
    ITO EM_RST_RSRE  Move El1=S_SAL_REG_END  Exit=SAL_REG_END
    ITO EM_RST_RL    Move El1=S_RA_LINK      Exit=RA_LINK
    RREDI EM_RST_r

── EMIT_JR_DISPATCH: emit %L_jrdispatch switch block ──────────
EMIT EMIT_JR_DISPATCH SF_JR_HEAD
EMIT EJR_LOAD SF_JR_LOAD
CLEAR EJR_SCAN RA_I
JEQ EJR_WLOOP RA_I RA_VS_WORDS   EJR_TAIL
ITO EJR_WADDR  Add   El1=RA_VS_BASE El2=RA_I Exit=RA_TMP
ITO EJR_WLOAD  Read El1=RA_TMP           Exit=RA_JR_WORD
JZ EJR_WNZ RA_JR_WORD EJR_WNXT
CLEAR EJR_BLOOP RA_JR_BI
JEQ EJR_BLOOPCK RA_JR_BI C_64 EJR_WNXT
ITO EJR_BMASK  Left   El1=C_1 El2=RA_JR_BI Exit=RA_TMP2
ITO EJR_BTEST  And   El1=RA_JR_WORD El2=RA_TMP2 Exit=RA_TMP2
JZ EJR_BCHK RA_TMP2 EJR_BNXT
ITO EJR_NCOMP  Left  El1=RA_I El2=C_6    Exit=RA_JR_LID  /word_idx * 64 = word_idx << 6
ITO EJR_NADD   Add   El1=RA_JR_LID El2=RA_JR_BI Exit=RA_JR_LID
EMIT EJR_ARMPFX SF_JR_ARM_PFX
EMITI EJR_ARMNID RA_JR_LID
EMIT EJR_ARMMID SF_JR_ARM_MID
EMITI EJR_ARMLNID RA_JR_LID
PUTBYTE EJR_ARMLNL LF
ITO EJR_BNXT   Add   El1=RA_JR_BI El2=C_1 Exit=RA_JR_BI
ITO EJR_BNXTL  Jump  Exit=EJR_BLOOPCK
ITO EJR_WNXT   Add   El1=RA_I El2=C_1     Exit=RA_I
ITO EJR_WNXTL  Jump  Exit=EJR_WLOOP
EMIT EJR_TAIL SF_JR_TAIL
RREDI EJR_RET_r

── BFS_ENQ: enqueue RA_TMP if not visited ────────────────────
/Non-leaf — RA_LINK is saved/restored automatically by the call stack.
/Guard: only enqueue ITO luxs (Op != 0). Data luces have Op=0 → undefined labels in JR_DISPATCH.
ITO BFS_ENQ   Add     El1=RA_TMP         El2=C_1           Exit=RA_TMP2   /addr+SLOT_OP
ITO BEN_OPRD    Read El1=RA_TMP2                             Exit=RA_TMP2   /read Op
ITO BEN_OPTEST  JumpIf  El1=RA_TMP2        Exit=BEN_IS_ITO
RREDI BEN_SKIP_r
CHAIN
    BEN_IS_ITO  Move    El1=RA_TMP         Exit=RA_VS_ID
        RVOCA BEN_VTR VS_TEST_SET
    BEN_TESTED  JumpIf  El1=RA_FLAG        Exit=BEN_ALREADY
    BEN_BQAR    Move    El1=RA_BQ_TAIL     Exit=RA_TMP
        RVOCA BEN_BQARJ BQ_ADDR
    BEN_BQDONE  Write El1=RA_TMP        El2=RA_VS_ID
    BEN_TINC    Add     El1=RA_BQ_TAIL     El2=C_1 Exit=RA_BQ_TAIL
        RREDI BEN_RETJ
RREDI BEN_ALREADY
── BFS_LOOP: dequeue + emit block + enqueue successors ───────
/RA_LINK is saved/restored automatically by the call stack./
NEWREF BFS_LOOP_SAVE BFS_LOOP  /alias: external entry point/
JEQ BFS_LOOP RA_BQ_HEAD RA_BQ_TAIL BFS_DONE
ITO BFS_DEQR    Move    El1=RA_BQ_HEAD     Exit=RA_TMP
RVOCA BFS_DEQBQRJ BQ_ADDR
ITO BFS_DEQDONE Read El1=RA_TMP         Exit=RA_INSTR
ITO BFS_HINC    Add     El1=RA_BQ_HEAD     El2=C_1 Exit=RA_BQ_HEAD
ITO EB_SAVE_RET Move    El1=BFS_SCAN_LUX  Exit=RA_EB_OUTER  /save return point for EMIT_BLOCK
ITO BFS_EMITJ   Jump    Exit=EMIT_BLOCK
NOLINK
//Enqueue successors: read SLOT_NEXT directly + check SLOT_EXIT for Jump/JumpIf.
Only Jump and JumpIf have Exit = static ITO address → must follow for correct CFG.
Voca/JumpReg/Redi Exit is runtime (RA_LINK or register) → must NOT follow.
ITO BFS_SCAN_LUX Add   El1=RA_INSTR El2=C_5  Exit=RA_TMP   /SLOT_NEXT=5
ITO BFS_RD_NXT   Read El1=RA_TMP            Exit=RA_TMP    /read next ITO addr
JZ    BFS_NXTCHK  RA_TMP BFS_SCAN_EXIT                     /0 (NOLINK) → skip to exit check
/RA_TMP already holds SLOT_NEXT addr — enqueue directly
RCALL_AT BFS_ENQ_NJ BFS_ENQ BFS_SCAN_EXIT
//Check SLOT_EXIT for Jump / JumpIf, and SLOT_E1 for Voca.
Jump/JumpIf: Exit = static destination block address.
Voca: El1 = static target subroutine address (Exit=RA_LINK is dynamic, skip it).
Redi/JumpReg: fully dynamic → skip.
NOLINK
ITO BFS_SCAN_EXIT Add  El1=RA_INSTR El2=C_1  Exit=RA_TMP2  /SLOT_OP=1
ITO BFS_EXIT_ROP  Read El1=RA_TMP2          Exit=RA_TMP2   /read op_id

SWITCH RA_TMP2
    Jump JumpIf > BFS_EXIT_DO
    Voca        BFS_VOCA_DO

/Not a CFG-branching op → back to BFS_LOOP
ITO BFS_EXIT_SKIP Jump  Exit=BFS_LOOP
NOLINK
/Jump/JumpIf: enqueue SLOT_EXIT (slot 4)
ITO BFS_EXIT_DO   Add  El1=RA_INSTR El2=C_4  Exit=RA_TMP    /SLOT_EXIT=4
ITO BFS_EXIT_RD   Read El1=RA_TMP           Exit=RA_TMP     /read exit target addr
JZ    BFS_EXIT_NZ  RA_TMP BFS_LOOP                          /0 → skip
RCALL_AT BFS_EXIT_EJ BFS_ENQ BFS_LOOP
NOLINK
/Voca: enqueue SLOT_E1 (slot 2) — the static call target
ITO BFS_VOCA_DO   Add  El1=RA_INSTR El2=C_2  Exit=RA_TMP    /SLOT_E1=2
ITO BFS_VOCA_RD   Read El1=RA_TMP           Exit=RA_TMP     /read e1 = target addr
JZ    BFS_VOCA_NZ  RA_TMP BFS_LOOP                          /0 → skip
RCALL_AT BFS_VOCA_EJ BFS_ENQ BFS_LOOP
/BFS_DONE: queue empty — return (automatic stack restores RA_LINK).
NOLINK
RREDI BFS_DONE
── EMIT_BLOCK: emit one basic block for RA_INSTR ─────────────
/Check Op via direct SLOT_OP read (compact ITO slot 1, not a lumen)
ITO EMIT_BLOCK   Add     El1=RA_INSTR  El2=C_1   Exit=RA_TMP
ITO EB_RDOP      Read El1=RA_TMP                 Exit=RA_FLAG
ITO EB_PREOPDJ   JumpIf  El1=RA_FLAG             Exit=EB_EMIT_LBL
//Lux without Op: not an instruction, skip without clearing visited bit.
(Clearing the bit caused infinite BFS loops — a Lux can be re-enqueued
after its bit is cleared, leading EMIT_BLOCK to clear it again, repeat.)
RREDI EB_EARLY_RET_r

PUTBYTE EB_EMIT_LBL LF
PUTBYTE EB_LBL_L ASCII_L
EMITI EB_LNID RA_INSTR
PUTBYTE EB_LCOL COLON
PUTBYTE EB_LNL LF
/Read Op directly from SLOT_OP (slot 1). At this point Op is known non-zero.
ITO EB_FINDOP    Add     El1=RA_INSTR  El2=C_1   Exit=RA_TMP
ITO EB_FD_RD     Read El1=RA_TMP                 Exit=RA_OP_ID
CLEAR EB_NO_OP RA_A1
CLEAR EB_CLR2 RA_A2
CLEAR EB_CLR3 RA_EXIT_N
CLEAR EB_CLR5 RA_NXT_N
FOR RA_SSA_FR RA_SSA_FR2 RA_SSA_RESULT RA_SSA_V1 RA_SSA_V2
    ITO EB_CLR_{N} Write El1={X} El2=C_0  /zero SSA slot

/SSA cache: always invalidated per BB (JR-dispatch makes every BB a potential entry).
ITO EB_SSA_GEN_INC Add El1=RA_SSA_GEN El2=C_1 Exit=RA_SSA_GEN
CLEAR EB_SSA_GEN_DONE RA_TMP
//Read El1/El2/Exit/Next from compact ITO fixed slots (O(1) direct reads).
RA_EXIT_N = SLOT_EXIT: exit is unified — for jumps it is destination block,
for arithmetic/move it is target register. RO_LDEST only appears in jump templates.

CHAIN
    EB_LOADOPS   Add   El1=RA_INSTR El2=C_2  Exit=RA_TMP
    EB_LD_A1     Read El1=RA_TMP            Exit=RA_A1
    EB_LD_A2P    Add   El1=RA_INSTR El2=C_3  Exit=RA_TMP
    EB_LD_A2     Read El1=RA_TMP            Exit=RA_A2
    EB_LD_EXITP  Add   El1=RA_INSTR El2=C_4  Exit=RA_TMP
    EB_LD_EXIT   Read El1=RA_TMP            Exit=RA_EXIT_N
    EB_LD_NXTP   Add   El1=RA_INSTR El2=C_5  Exit=RA_TMP
    EB_LD_NXT    Read El1=RA_TMP            Exit=RA_NXT_N

/── Equal+JumpIf fusion check ────────────────────────────────────────────────
/If op==JumpIf AND RA_SSA_CMP!=0 AND RA_A1==RA_JEQ_FLAG:
/  use JumpIfCmp rule (direct br i1 {cmp} without load+icmp ne)
/If op==JumpIf AND RA_SSA_ARITH_TGT!=0 AND RA_A1==RA_SSA_ARITH_TGT:
/  use JumpIfNZ rule (icmp ne {arith}, 0 + br)
/Otherwise: clear pending state
JEQ EB_OPJIFCK RA_OP_ID JumpIf EB_FUSE_ISJI
/Not JumpIf: clear pending cmp and arith state
CLEAR EB_CLR_CMP RA_SSA_CMP
CLEAR EB_CLR_AI RA_SSA_ARITH_IDX
CLEAR EB_CLR_AT RA_SSA_ARITH_TGT
ITO EB_NOFUSE    Jump Exit=EB_FINDTMPL
NOLINK
JZ EB_FUSE_CMPCZ RA_SSA_CMP EB_FUSE_ARITH   /no pending cmp → try arith
JEQ EB_FUSE_A1CK RA_A1 RA_JEQ_FLAG EB_FUSE_DO_CMP
ITO EB_FUSE_NOTA1 Jump Exit=EB_FUSE_ARITH
NOLINK
ITO EB_FUSE_DO_CMP  Add  El1=RA_RT_BASE El2=JumpIfCmp Exit=RA_TMP
ITO EB_FUSE_RD_CMP  Read El1=RA_TMP                   Exit=RA_RULE
ITO EB_FUSE_GO_CMP  Jump Exit=EB_DO_EMIT
/Try arith fusion
NOLINK
JZ EB_FUSE_ARITH RA_SSA_ARITH_TGT EB_FINDTMPL
JEQ EB_FUSE_A1AT RA_A1 RA_SSA_ARITH_TGT EB_FUSE_DO_NZ
ITO EB_FUSE_NOAT Jump Exit=EB_FINDTMPL
NOLINK
ITO EB_FUSE_DO   Add  El1=RA_RT_BASE El2=JumpIfNZ Exit=RA_TMP
ITO EB_FUSE_RD   Read El1=RA_TMP                  Exit=RA_RULE
ITO EB_FUSE_DO_NZ Jump Exit=EB_DO_EMIT

── EB_FINDTMPL: O(1) rule lookup via pre-built rule table ────
ITO EB_FINDTMPL Add     El1=RA_RT_BASE El2=RA_OP_ID Exit=RA_TMP
ITO EB_FT_RD    Read El1=RA_TMP                   Exit=RA_RULE
/auto-Next falls through to JZ EB_EMIT_TMPL (EB_FT_FOUND Jump was redundant)
/EB_DO_EMIT: entry point when RA_RULE is already set (e.g. fusion path)
NOLINK
JZ EB_DO_EMIT RA_RULE EB_FALL
RVOCA EB_PRELOADS EMIT_PRELOADS
ITO EB_EMIT_TMPL_DONE Read El1=RA_RULE Exit=RA_TW_LUX /Rule.word = first byte Lux
RVOCA EB_ETMPLR EMIT_TMPL
/After emit: if rule HasCmpResult → save RA_SSA_FR → RA_SSA_CMP for fusion with next JumpIf
WALK_ONE EB_HASCMP RA_RULE HasCmpResult
JZ EB_HASCMP_CHK RA_SR_OUT EB_HASARITH  /no HasCmpResult → check arith
/Has pending icmp i1: save RA_SSA_FR (fresh SSA index) into RA_SSA_CMP
ITO EB_SAVE_CMP  Read El1=RA_SSA_FR Exit=RA_SSA_CMP
/Clear arith state — cmp replaces arith
CLEAR EB_CLR_ARITH RA_SSA_ARITH_IDX
CLEAR EB_CLR_ARTGT RA_SSA_ARITH_TGT
ITO EB_POST_CMP  Jump Exit=EB_FALL   /continue to Terminates check
/Check HasArithResult: save RA_SSA_RESULT + RA_EXIT_N as pending arith for JumpIfNZ
NOLINK
WALK_ONE EB_HASARITH RA_RULE HasArithResult
JZ EB_HASARITH_CHK RA_SR_OUT EB_FALL  /no HasArithResult → continue
ITO EB_SAVE_ARITH Read El1=RA_SSA_RESULT Exit=RA_SSA_ARITH_IDX
ITO EB_SAVE_ARTGT Move El1=RA_EXIT_N     Exit=RA_SSA_ARITH_TGT
/Clear cmp state — arith replaces cmp
CLEAR EB_CLR_CMPF RA_SSA_CMP
NOLINK
ITO EB_POST_ARITH Jump Exit=EB_FALL   /continue to Terminates check
NOLINK
WALK_ONE EB_FALL RA_RULE Terminates
JZ EB_FALL_DN RA_SR_OUT EB_DO_FALL

RREDI EB_NO_FALL_r  /has Terminates → no fallthrough
JZ EB_DO_FALL RA_NXT_N EB_EMIT_UNREACH
EMIT EB_BRBR SF_BR
EMITI EB_BRNID RA_NXT_N
PUTBYTE EB_BRNL LF
EMIT EB_EMIT_UNREACH SF_UNREACHABLE
//(EB_NO_FALL moved above; EB_EMIT_UNREACH falls through to EB_NO_FALL if needed)
Second JumpReg for the fallthrough path after emitting unreachable/br:
RREDI EB_FALL_END_r

── EMIT_PRELOADS: emit GEP+load for each operand ─────────────
JZ EMIT_PRELOADS RA_A1 EPL_A2
ITO EPL_A1 Move El1=RA_A1 Exit=RA_TMP
RVOCA EPL_A1R PRELOAD_ARG
ITO EPL_A1SAVE Write El1=RA_SSA_V1 El2=RA_SSA_OUT  /write ssa_idx into DATA slot, preserve pointer
JZ EPL_A2 RA_A2 EPL_EXIT
ITO EPL_A2B Move El1=RA_A2 Exit=RA_TMP
RVOCA EPL_A2R PRELOAD_ARG
ITO EPL_A2SAVE Write El1=RA_SSA_V2 El2=RA_SSA_OUT  /write ssa_idx into DATA slot, preserve pointer
JZ EPL_EXIT RA_EXIT_N EPL_SYSCHK
//Target: inline GEP — no separate %ptN. ETH_PTR_TGT emits getelementptr inbounds inline.
RA_EXIT_N already set by EB_STEXIT. Nothing to emit here.
JEQ EPL_SYSCHK RA_OP_ID Exire EPL_SYS_A0
ITO EPL_NOSYS   Jump  Exit=EPL_DONE

//Exire preload: 5 explicit PRELOAD_ARG calls for SC_A0..SC_A3, SC_NR.
Use SC_*_REF (stable SETREF pointers) so RA_TMP = SC_*_id (Lux ID),
not heap[SC_*] (runtime value which changes after Move SC_* writes into it).
FOR A0 A1 A2 A3 NR
    ITO EPL_SYS_{X}   Move El1=SC_{X}_REF Exit=RA_TMP
    RVOCA EPL_SYS_{X}R  PRELOAD_ARG

RREDI EPL_DONE
── PRELOAD_ARG: emit inline-GEP load for one operand Lux ──────────
//Emits: "  %vN = load i64, ptr getelementptr inbounds ([S x i64], ptr @heap, i64 0, i64 <lid>)"
No separate %ptN register — GEP is inlined into load operand (valid LLVM IR).
NOLINK
NORESTORE
SAVE PRELOAD_ARG
    ITO PA_SETLID  Move El1=RA_TMP Exit=RA_SSA_LID
    ITO PA_SAVNID  Move El1=RA_TMP Exit=RA_PA_LID
    RCALL_AT PA_GS GET_SSA PA_ISNEW
    JZ PA_ISNEW RA_SSA_HIT PA_EMIT_ILOAD
    ITO PA_SKIP  Move El1=S_RA_LINK Exit=RA_LINK
    RREDI PA_SKIP_r
    NOLINK
    EMIT PA_EMIT_ILOAD SF_INDENT
    EMIT PA_LLV        SF_VPX
    EMITI PA_LLSSA     RA_SSA_OUT
    EMIT PA_LLIGPRE    SF_LOAD_IGEP_PRE
    EMITI PA_LLTXSZ    K_AETHER_SIZE
    EMIT PA_LLMID      SF_GEP_MID
    EMITI PA_LLID      RA_PA_LID
    EMIT PA_LLCLOSE    SF_IGEP_CLOSE_NL
    ITO PA_RET  Move El1=S_RA_LINK Exit=RA_LINK
    RREDI PA_RET_r

── GET_SSA: lookup or assign SSA index for Lux RA_SSA_LID ──
//Direct-map: slot = RA_SSA_BASE + lux_id. Format: (gen << 32) | ssa_idx.
Hit: slot_gen == RA_SSA_GEN. Miss: allocate new SSA index, write slot.
Leaf (no sub-calls → no OUTER needed).
IN:  RA_SSA_LID = Lux ID to look up
OUT: RA_SSA_OUT = SSA index (existing or newly assigned)
RA_SSA_HIT = 0 if new (slot was miss), nonzero if found
NOLINK
ITO GET_SSA      Add     El1=RA_SSA_BASE   El2=RA_SSA_LID  Exit=RA_TMP
ITO GS_LOAD      Read El1=RA_TMP        Exit=RA_TMP2
ITO GS_EXTGEN    Right     El1=RA_TMP2       El2=C_32        Exit=RA_TMP3
ITO GS_HITEQ     Equal El1=RA_TMP3     El2=RA_SSA_GEN  Exit=RA_SSA_HIT
JZ    GS_HITCHK    RA_SSA_HIT GS_NEW
/Hit: extract ssa_idx from lower 32 bits
ITO GS_FOUND     And     El1=RA_TMP2       El2=MASK_LOW32   Exit=RA_SSA_OUT
RREDI GS_FRET
/Miss: allocate new SSA index, write slot
ITO GS_NEW       Add     El1=RA_SSA_CTR    El2=C_1          Exit=RA_SSA_CTR
ITO GS_NASSIGN   Move    El1=RA_SSA_CTR    Exit=RA_SSA_OUT
ITO GS_PACKGEN   Left     El1=RA_SSA_GEN    El2=C_32         Exit=RA_TMP2
ITO GS_PACKIDX   Or      El1=RA_TMP2       El2=RA_SSA_CTR   Exit=RA_TMP2
ITO GS_NSTORE    Write El1=RA_TMP        El2=RA_TMP2
RREDI GS_NRET

── EMIT_TMPL: read packed-string template byte by byte ──
/IN: RA_TW_LUX = first lux of packed template (Rule.word).
/Non-leaf — RA_LINK is saved/restored automatically by the call stack.
/Packed string: bytes packed little-endian into u64 luces (DATA_LUX_MIN=2 apart).
/Escape bytes 0x01-0x1F = placeholder dispatch. 0x0A = newline. 0x20+ = literal ASCII.
/Null byte (0x00) terminates.
NEWREF EMIT_TMPL ET_LOAD  /alias/

/── Load next lux ─────────────────────────────────────────────────────────
NOLINK
ITO ET_LOAD        Read  El1=RA_TW_LUX       Exit=RA_ET_WORD   /word = aether[lux]
JZ    ET_LOADCHK   RA_ET_WORD ET_DONE                          /word==0 → done
CLEAR ET_SHIFTINIT RA_ET_SHIFT                                  /shift = 0

/── Extract one byte from current word ─────────────────────────────────────
NOLINK
ITO ET_EXTRACT     Right El1=RA_ET_WORD       El2=RA_ET_SHIFT   Exit=RA_ET_BYTE
ITO ET_MASKBYTE    And   El1=RA_ET_BYTE       El2=C_255         Exit=RA_ET_BYTE
JZ    ET_BYTECHECK RA_ET_BYTE ET_DONE                           /byte==0 → null terminator

/Dispatch: byte >= 0x20 → literal, byte < 0x20 → escape
JEQ   ET_IS20      RA_ET_BYTE C_32            ET_LITERAL        /== 0x20 → literal (space)
ITO ET_LTCHECK     Right El1=RA_ET_BYTE       El2=C_5           Exit=RA_TMP   /byte >> 5
JZ    ET_ESCCHECK  RA_TMP ET_ESCAPE                              />> 5 == 0 → byte < 0x20

/── Literal byte (0x20..0xFF): emit directly ────────────────────────────────
NOITO
    ET_LITERAL     Move  El1=RA_ET_BYTE       Exit=RA_BYTE
        RVOCA ET_LIT_PUT PUT_BYTE
        ITO ET_LIT_JN      Jump  Exit=ET_ADVANCE
    ET_ESCAPE_LB   Jump  Exit=ET_ADVANCE_FIRST
    ET_ESCAPE      Jump  Exit=ET_ADVANCE

SWITCH RA_ET_BYTE
    C_1   ETH_V1
    C_2   ETH_V2
    C_3   ETH_RESULT
    C_4   ETH_XLEN_SV
    C_5   ETH_FRESH
    C_6   ETH_FRESH2
    C_7   ETH_PTR_TGT_SV
    C_8   ETH_LDEST_SV
    C_9   ETH_LNEXT_SV
    C_10  ETH_CMP
    C_11  ETH_LNEXT_ID_SV
    C_12  ETH_TOTALSZ_SV
    C_13  ETH_ARITH
    C_14  ETH_FLB_SV
    C_15  ETH_VX0
    C_16  ETH_VX1
    C_17  ETH_VX2
    C_18  ETH_VX3
    C_19  ETH_VX8

NOITO
    ET_ADVANCE     Add   El1=RA_ET_SHIFT      El2=C_8           Exit=RA_ET_SHIFT
        JEQ   ET_LUXCHK   RA_ET_SHIFT C_64           ET_NEXT_LUX
        ITO ET_LOOPBACK    Jump  Exit=ET_EXTRACT
    ET_NEXT_LUX   Add   El1=RA_TW_LUX        El2=C_2           Exit=RA_TW_LUX
        ITO ET_LUXLOOP    Jump  Exit=ET_LOAD
    ET_ADVANCE_FIRST Jump Exit=ET_ADVANCE

/── ETH_* handlers ──────────────────────────────────────────────────────────
/Each handler emits its value then jumps to ET_ADVANCE to consume the escape byte.

NOITO
    ETH_V1       Move  El1=RA_SSA_V1   Exit=RA_ESAR_REG
        RVOCA ETH_V1J      ET_EMIT_SSA_REG
        ITO ETH_V1_JN    Jump  Exit=ET_ADVANCE
    ETH_V2       Move  El1=RA_SSA_V2   Exit=RA_ESAR_REG
        RVOCA ETH_V2J      ET_EMIT_SSA_REG
        ITO ETH_V2_JN    Jump  Exit=ET_ADVANCE
    ETH_RESULT   Move  El1=RA_SSA_RESULT Exit=RA_FAL_REG
        RVOCA ETH_RES_J    ET_PH_FRESH_ALLOC
        ITO ETH_RES_JN   Jump  Exit=ET_ADVANCE
    ETH_FRESH    Move  El1=RA_SSA_FR     Exit=RA_FAL_REG
        RVOCA ETH_FR_J     ET_PH_FRESH_ALLOC
        ITO ETH_FR_JN    Jump  Exit=ET_ADVANCE
    ETH_FRESH2   Move  El1=RA_SSA_FR2    Exit=RA_FAL_REG
        RVOCA ETH_FR2_J    ET_PH_FRESH_ALLOC
        ITO ETH_FR2_JN   Jump  Exit=ET_ADVANCE
    ETH_CMP      Move  El1=RA_SSA_CMP    Exit=RA_ESAR_REG
        RVOCA ETH_CMP_J    ET_EMIT_SSA_REG
        ITO ETH_CMP_JN   Jump  Exit=ET_ADVANCE

NOITO
    ETH_ARITH    Move  El1=RA_SSA_ARITH_IDX Exit=RA_ESAR_REG
        RVOCA ETH_ARITH_J  ET_EMIT_SSA_REG
        ITO ETH_ARITH_JN Jump  Exit=ET_ADVANCE
    ETH_PTR_TGT_SV Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
        EMIT  ETH_PTR_TGT  SF_PTR_IGEP
        EMITI ETH_PT_SZ    K_AETHER_SIZE
        EMIT  ETH_PT_MID   SF_GEP_MID
        EMITI ETH_PT_I     RA_EXIT_N
        EMIT  ETH_PT_CL    SF_IGEP_CLOSE
        ITO ETH_PT_JN    Jump  Exit=ETH_TW_RESTORE
    ETH_LDEST_SV Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
        EMIT  ETH_LDEST    SF_LBL
        EMITI ETH_LD_I     RA_EXIT_N
        ITO ETH_LD_JN    Jump  Exit=ETH_TW_RESTORE
    ETH_LNEXT_SV Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
        EMIT  ETH_LNEXT    SF_LBL
        EMITI ETH_LN_I     RA_NXT_N
        ITO ETH_LN_JN    Jump  Exit=ETH_TW_RESTORE
    ETH_LNEXT_ID_SV Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
        EMITI ETH_LN_ID_I  RA_NXT_N
        ITO ETH_LN_ID_JN Jump  Exit=ETH_TW_RESTORE
    ETH_XLEN_SV  Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
        EMITI ETH_XLEN     K_XLEN
        ITO ETH_XL_JN    Jump  Exit=ETH_TW_RESTORE
    ETH_TOTALSZ_SV Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
        EMITI ETH_TOTALSZ  K_AETHER_SIZE
        ITO ETH_TS_JN    Jump  Exit=ETH_TW_RESTORE
    ETH_FLB_SV   Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
        EMITI ETH_FLB      C_4
        ITO ETH_FL_JN    Jump  Exit=ETH_TW_RESTORE

//ETH_VX0..ETH_VX8: emit %v + GET_SSA(SC_Ax)//
FOR SC_A0_REF SC_A1_REF SC_A2_REF SC_A3_REF SC_NR_REF
    NOLINK
    ITO ETH_VX{N}  Move  El1={X} Exit=RA_SSA_LID
    RCALL_AT ETH_VX{N}J  GET_SSA ETH_VX_SAVE

NOLINK
ITO ETH_VX_SAVE  Move  El1=RA_TW_LUX    Exit=RA_ETH_TW_SAVED
EMIT  ETH_VX_EMIT  SF_VPX
EMITI ETH_VX_I     RA_SSA_OUT
ITO ETH_VX_JN    Jump  Exit=ETH_TW_RESTORE

/── ETH_TW_RESTORE: shared restore+advance for ETH_* save-pattern handlers ──
/IN:  RA_ETH_TW_SAVED = previously saved RA_TW_LUX value.
/Restores RA_TW_LUX then continues at ET_ADVANCE.
NOLINK
ITO ETH_TW_RESTORE Move El1=RA_ETH_TW_SAVED Exit=RA_TW_LUX
ITO ETH_TW_JN      Jump Exit=ET_ADVANCE

RREDI ET_DONE
── RETURN ADDRESS SELF-REFERENCES ────────────────────────────

── ENTRY POINT LINK ──────────────────────────────────────────
//P0_NID is the compiler ITO entry. SelfYaku prevents P0_SCAN
(which checks Entry→Yaku, not Entry→SelfYaku) from compiling
the compiler itself. ITO luxs can carry extra LINK lumen safely —
interpreter scans for rel=Next, not pc+fixed_size.
LINK P0_NID Entry SelfYaku

LINK EB_SAVE_RET Next BFS_EMITJ
//RT_BUILD_ENTRY is the RCALL_AT landing for P0_INIT_C → INIT_BFS.
NEWREF SelfYaku
This explicit Next link ensures BFS reaches it statically (NOLINK breaks auto-Next).
LINK P0_INIT_C Next RT_BUILD_ENTRY
/── Stubs for unimplemented functions ─────────────────────────
/These are entry points of functions not yet written.
/Declared as NEW to silence Wave-B Unknown name errors.
/Will be replaced with real implementations.
/EM_NOOP is now a real ITO lux (merged restore path above)
/EM_VISITED guard: Wave-B needs this for forward refs in indented SAVE blocks
NEW EM_VISITED
NEW EF_VSCLEAR
NEW EMIT_FUNC_SECONDARY
NEW RT_BUILD_ENTRY
NEW RTB_LUX_BODY
NEW P0_COLLECT_BODY
