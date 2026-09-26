============================================================
//
yaku.re: BFS + rule dispatch + LLVM IR emit

NOTE: RCALL_AT calls in this file are builder-class macros — they are
wired by LOAD_MAIN (see BUGS.md Pattern N). Until self-hosting is reached,
the compiler entry P0_NID produces 0 bytes of output.
//
============================================================

── COMPILER RELATIONS ────────────────────────────────────────
//
ForType:    Yaku_X --ForType--> AspectLux  (maps op → rule)
Terminates: Yaku_X --Terminates--> Yaku    (marks terminator rules)
//

── ALU RULES ────────────────────────────────────────────────
YAKU Add HasArithResult
    {result} = add i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU Sub HasArithResult
    {result} = sub i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU Mul HasArithResult
    {result} = mul i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU Div HasArithResult
    {result} = sdiv i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU Rem HasArithResult
    {result} = srem i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU And HasArithResult
    {result} = and i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU Or HasArithResult
    {result} = or i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU Xor HasArithResult
    {result} = xor i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU Not HasArithResult
    {result} = xor i{XLEN} {v1}, -1
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU Left HasArithResult
    {result} = shl i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU Right HasArithResult
    {result} = lshr i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU ARight HasArithResult
    {result} = ashr i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU UDiv HasArithResult
    {result} = udiv i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU URem HasArithResult
    {result} = urem i{XLEN} {v1}, {v2}
    store i{XLEN} {result}, ptr {ptr_tgt}

── COMPARISON RULES ─────────────────────────────────────────
YAKU Equal HasCmpResult
    {fresh} = icmp eq i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU Less HasCmpResult
    {fresh} = icmp slt i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU ULess HasCmpResult
    {fresh} = icmp ult i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU Greater HasCmpResult
    {fresh} = icmp slt i{XLEN} {v2}, {v1}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU UGreater HasCmpResult
    {fresh} = icmp ugt i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU NotEqual HasCmpResult
    {fresh} = icmp ne i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU LessOrEqual HasCmpResult
    {fresh} = icmp sle i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU ULessOrEqual HasCmpResult
    {fresh} = icmp ule i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU GreaterOrEqual HasCmpResult
    {fresh} = icmp sge i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}
YAKU UGreaterOrEqual HasCmpResult
    {fresh} = icmp uge i{XLEN} {v1}, {v2}
    {result} = zext i1 {fresh} to i{XLEN}
    store i{XLEN} {result}, ptr {ptr_tgt}

── CONTROL FLOW RULES (terminators) ─────────────────────────
YAKU End Terminates
    ret i{XLEN} 0
    
//
LATENT ISSUE (see runtime/registers.re for the RCALL_AT counterpart): the
interpreter's Redi/Voca also pop/push RA_LINK on the automatic call
stack (RA_SP), but these LLVM-IR templates don't yet emit that —
{RA_SP_ID}/{RA_FRAME_SIZE_ID} placeholder escapes don't exist in the
current substitution system. Compiled output would diverge from the
interpreter for any nested call. Not yet an active bug: this whole
compiler path is unwired (0 bytes output in both old and new builds).
Needs: add RA_SP_ID/RA_FRAME_SIZE_ID escapes, then update both
templates below to pop-before-jump (Redi) and push-before-jump (Voca).
//

YAKU Redi Terminates
    {fresh} = load i{XLEN}, ptr getelementptr inbounds ([{TOTAL_SIZE} x i{XLEN}], ptr @heap, i{XLEN} 0, i{XLEN} {RA_LINK_ID})
    store i{XLEN} {fresh}, ptr @jr_slot
    br label %L_jrdispatch
YAKU Jump Terminates
    br label {label_dest}
YAKU JumpIf Terminates
    {fresh} = icmp ne i{XLEN} {v1}, 0
    br i1 {fresh}, label {label_dest}, label {label_next}

//
JumpIfCmp: used when E1==RA_JEQ_FLAG and a pending i1 result is in RA_SSA_CMP.
Uses {cmp} (escape 0x0A) = pending icmp i1 SSA index directly — no load, no icmp ne.
This fuses Equal/Less/ULess/Greater + JumpIf into a single icmp + br.
System: any op with HasCmpResult lumen auto-fuses with following JumpIf via this rule.
//

SET JumpIfCmp
YAKU JumpIfCmp Terminates
    br i1 %v{cmp}, label {label_dest}, label {label_next}

//
JumpIfNZ: used when E1 ptr_tgt matches last arithmetic op (HasArithResult).
Uses {arith} (escape C_13) = i64 SSA result from Add/Sub/Mul/etc.
Fuses arithmetic op + JumpIf into icmp ne + br without store+load.
//

SET JumpIfNZ
YAKU JumpIfNZ Terminates
    {fresh} = icmp ne i{XLEN} %v{arith}, 0
    br i1 {fresh}, label {label_dest}, label {label_next}

/I.3: Yaku_JumpReg — indirect jump, does NOT set RA_LINK (pure goto).
YAKU JumpReg Terminates
    store i{XLEN} {v1}, ptr @jr_slot
    br label %L_jrdispatch
/Yaku_Call — sets RA_LINK = NXT_N (return address) then jumps.
SET ForType
SET Terminates

//
HasCmpResult: marks ops that produce an i1 icmp result in {fresh}/RA_SSA_FR.
EMIT_BLOCK saves RA_SSA_FR → RA_SSA_CMP after emitting these ops.
Any op with this lumen auto-fuses with the following JumpIf (via JumpIfCmp).
//

SET HasCmpResult

//
HasArithResult: marks ops that produce an i64 arithmetic result in {result}/RA_SSA_RESULT.
EMIT_BLOCK saves RA_SSA_RESULT → RA_SSA_ARITH_IDX and ptr_tgt → RA_SSA_ARITH_TGT.
JumpIf with E1==RA_SSA_ARITH_TGT fuses into icmp ne {arith}, 0 + br (JumpIfNZ).
//

SET HasArithResult

//
Matches interpreter _voca's RA_LINK assignment (the RA_SP push is NOT
yet emitted here — see the LATENT ISSUE note on the Redi template above).
//

YAKU Voca Terminates
    store i{XLEN} {lnext_id}, ptr getelementptr inbounds ([{TOTAL_SIZE} x i{XLEN}], ptr @heap, i{XLEN} 0, i{XLEN} {RA_LINK_ID})
    store i{XLEN} {v1}, ptr @jr_slot
    br label %L_jrdispatch

── AETHER / REGISTER RULES ───────────────────────────────────
YAKU Move
    store i{XLEN} {v1}, ptr {ptr_tgt}

//
I.1: Yaku_Read handles Read + Load — both map to same IR.
Uses DEF_RULE_ALIAS so both ops map to same rule. Template uses {fresh} for GEP.
//

YAKU Read Terminates Write
    Load {fresh} = getelementptr [{TOTAL_SIZE} x i{XLEN}], ptr @heap, i{XLEN} 0, i{XLEN} {v1}
    {result} = load i{XLEN}, ptr {fresh}
    store i{XLEN} {result}, ptr {ptr_tgt}

/I.2: Yaku_Write handles Write + Store — no target write needed.
YAKU Write Terminates Read
    Store {fresh} = getelementptr [{TOTAL_SIZE} x i{XLEN}], ptr @heap, i{XLEN} 0, i{XLEN} {v1}
    store i{XLEN} {v2}, ptr {fresh}

── SYSTEM INTERFACE RULES ────────────────────────────────────
YAKU Exire
    call i{XLEN} @_sys(i{XLEN} {v_x0}, i{XLEN} {v_x1}, i{XLEN} {v_x2}, i{XLEN} {v_x3}, i{XLEN} {v_x8})

============================================================
============================================================

── SC_*_REF: stable Lux-ID pointers for syscall code generation ─
//
Problem: SC_A0..SC_NR serve dual roles:
(1) Runtime registers: compiler writes fd/buf/count into them for PB_FLUSH.
After write: SC_A0.word = 1 (stdout fd), not SC_A0_id anymore.
(2) Code-gen markers: EMIT_PRELOADS/ETH_VX* need the Lux *ID* of SC_Ax
to call PRELOAD_ARG and emit "load heap[SC_Ax_id]" in generated IR.
Solution (Idem, Aspect #25, this session): both call sites now compute
SC_Ax's own Lux ID fresh, directly, with no pre-declared _REF holder.
EPL_SYS_*: Idem El1=SC_A0 Exit=RA_TMP  gives RA_TMP = SC_A0_id (stable).
ETH_VX*:   Idem El1=SC_A0 Exit=RA_SSA_LID  for GET_SSA lookup.
Both sides use the same stable ID, so PRELOAD_ARG and ETH_VX* agree on SSA.
//

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

NEW ET_LOOP

NEW EMIT_SSA


── Registers ─────────────────────────────────────────────────
NEW RA_PA_LID
NEW RA_ETH_TW_SAVED  /ETH_* handlers: RA_TW_LUX saved before EMIT/EMITI clobbers it
NEW RA_ET_WORD  /current u64 word being scanned in packed template
NEW RA_ET_BYTE  /current extracted byte (0-255)
NEW RA_ET_SHIFT  /current bit shift within word (0,8,16,...,56)

NEW RA_NEXT_ID  /live value of K_CURSOR (current alloc_ptr = next free address)
NEW RA_ENTRY_LUX  /current compiler entry Lux
NEW RA_INSTR  /current instruction Lux being compiled
NEW RA_OP_ID  /Op primitive ID of current instruction
NEW RA_RULE  /Yaku_X Lux for current primitive



NEW RA_SSA_CTR  /monotonically increasing, 0 at function start

NEW RA_SSA_V1_DATA  /data slot for RA_SSA_V1 value
SET RA_SSA_V1 Value=RA_SSA_V1_DATA /pointer: word = ID of DATA
NEW RA_SSA_V2_DATA  /data slot for RA_SSA_V2 value
SET RA_SSA_V2 Value=RA_SSA_V2_DATA /pointer: word = ID of DATA
NEW RA_SSA_OUT  /SSA index for result register (output of PRELOAD_ARG)
NEW RA_SSA_RESULT_DATA  /data slot for RA_SSA_RESULT value
SET RA_SSA_RESULT Value=RA_SSA_RESULT_DATA /pointer: word = ID of DATA
NEW RA_SSA_FR_DATA  /data slot for {fresh} SSA value
SET RA_SSA_FR Value=RA_SSA_FR_DATA /pointer: word = ID of DATA
NEW RA_SSA_FR2_DATA  /data slot for {fresh2} SSA value
SET RA_SSA_FR2 Value=RA_SSA_FR2_DATA /pointer to FR2 data

NEW RA_A1  /El1 lux_id
NEW RA_A2  /El2 lux_id
NEW RA_EXIT_N  /Exit lux_id (SLOT_EXIT value)
NEW RA_NXT_N  /Next lux_id

NEW RA_LIMIT  /general loop limit

NEW RA_EM_I  /EMIT_MISSED outer loop counter (current Lux ID being examined)

/EB_OPS_BODY uses JEQ chain on rel addresses (no runtime table needed)

NEW RA_JR_BI  /inner bit-loop counter (0..63) for JR dispatch scan
NEW RA_JR_WORD  /current VS bitset word during JR dispatch scan
NEW RA_JR_LID  /computed Lux ID = word_idx*64 + bit_idx

NEW RA_RT_BASE  /base address of rule-lookup table (op_id → Yaku_X address)
NEW RA_VS_WORDS  /number of words in visited bitset = (alloc_ptr >> 6) + 1 (dynamic)
NEW RA_SSA_BASE  /base of SSA direct-map array (RA_NEXT_ID luces, indexed by Lux address)
NEW RA_SSA_GEN  /generation counter — incremented instead of clearing the SSA array
NEW RA_SSA_LID  /IN param for GET_SSA: Lux ID to look up (replaces RA_HT_HASH misuse)
NEW RA_SSA_HIT  /OUT param for GET_SSA: nonzero if slot was hit, 0 if new

//
RA_SSA_CMP: SSA index of last i1 icmp result from Equal/Less/ULess.
Persists across EMIT_BLOCK calls (NOT cleared per-BB) so JumpIf can use it directly.
0 = no pending comparison (cleared when any non-comparison NOVA is processed).
//

NEW RA_SSA_CMP

//
RA_SSA_ARITH_IDX: SSA index of last arithmetic op result (Add/Sub/etc.) — i64 type.
RA_SSA_ARITH_TGT: ptr_tgt addr that the arithmetic op wrote result to.
When JumpIf.E1 == RA_SSA_ARITH_TGT: fuse into icmp ne {result}, 0 + br (JumpIfNZ).
//

NEW RA_SSA_ARITH_IDX
NEW RA_SSA_ARITH_TGT
/RA_PC_BASE and RA_BEN_PC_ADDR removed — predecessor-count array eliminated.

── Subroutine registers ──────────────────────────────────────
NEW RA_FAL_REG  /ET_PH_FRESH_ALLOC: address of SSA register to check/assign
NEW RA_ESAR_REG  /ET_EMIT_SSA_REG:   address of SSA register whose value to emit as %vN

── Template escape bytes (see loader.py _TMPL_ESCAPE for mapping) ──
//
Packed string format used by EMIT_TMPL: escape bytes 0x01-0x13 for
placeholders ({v1}, {result}, {ptr_tgt}, etc — see parser.re's
PGB_PLACEHOLDER for the emitter side, SYNTH_YAKU in synth.re for the
builder side). This packed-string format replaced an earlier RO_* Lux
graph representation of templates -- RO_LITERAL/RO_V1/RO_RESULT/etc
were the old graph's per-node op types, each meant to be dispatched by
a walker that never existed here (the walker was never migrated with
the rest of NC_*/ET_DISPATCH's removal below). Confirmed zero
references anywhere in the project, Python included -- removed
2026-07-12, same family as parser.re's PS_DEF_BUILD_GRAPH removal.

NC_* removed — rule templates are packed strings (see above), not
strings requiring ET_DISPATCH's old string-based placeholder dispatch,
which is also gone.

SF_X_I64 value set by compiler_sf.re (generated by gen_compiler.py)
SF_FUNCOPEN value set by compiler_sf.re (generated by gen_compiler.py)
Secondary Entry: no @_init; name = @L<lux_id>

SF_GEP_MID value set by compiler_sf.re (generated by gen_compiler.py)
//

SET SF_VPX Value="%v"
SET SF_LBL Value="%L"
SET SF_INDENT Value="  "
SET SF_BR Value="  br label %L"
SET SF_HEAPDECL Value="@heap = global ["
NEW SF_X_I64
NEW SF_FUNCOPEN
SET SF_FUNCMAIN Value="main() {\nentry:\n  call void @reca_init()\n  br label %L"
SET SF_FUNCEND Value="}\n\n"
SET SF_FUNCSEC Value="reca_"
SET SF_FUNCSECBODY Value="() {\nentry:\n  br label %L"
NEW RA_EP_COUNT  /counts Entry→Yaku found; 0=first(@main), >0=secondary(@L<id>)
NEW SF_GEP_MID
── Inline GEP strings (SSA optimisation: no separate %ptN register) ─
Load: "  %vN = load i64, ptr getelementptr inbounds ([S x i64], ptr @heap, i64 0, i64 id)\n"
SET SF_LOAD_IGEP_PRE Value=" = load i64, ptr getelementptr inbounds (["
/Ptr for store template: "ptr getelementptr inbounds ([S x i64], ptr @heap, i64 0, i64 id)"
SET SF_PTR_IGEP Value="getelementptr inbounds (["
/Close with newline (for load/store lines) or without (for mid-line ptr_tgt)
SET SF_IGEP_CLOSE_NL Value=")\n"
SET SF_IGEP_CLOSE Value=")"
SET SF_DECLARE Value="@jr_slot = global i64 0\ndeclare i64 @write(i32, ptr, i64)\ndeclare i64 @read(i32, ptr, i64)\ndeclare i32 @openat(i32, ptr, i32, i32)\ndeclare i32 @close(i32)\ndeclare void @exit(i32)\n"

SET SF_UNREACHABLE Value="  unreachable\n"

── JumpReg dispatch emission ─────────────────────────────────
SET SF_JR_HEAD Value="\nL_jrdispatch:\n"
NEW SF_JR_LOAD
/SF_JR_LOAD value set by compiler_sf.re (generated by gen_compiler.py)
NEW SF_JR_ARM_PFX
/SF_JR_ARM_PFX value set by compiler_sf.re (generated by gen_compiler.py)
SET SF_JR_ARM_MID Value=", label %L"
NEW SF_JR_TAIL
/SF_JR_TAIL value set by compiler_sf.re (generated by gen_compiler.py)


── INIT_ARRAYS: allocate BFS queue, visited bitset, SSA direct-map array ─
//
Non-leaf — RA_LINK is saved/restored automatically by the call stack.

SSA table: direct-map array of K_AETHER_SIZE words, indexed by Lux ID.
Slot format: (generation << 32) | ssa_index.
"Clear" = increment RA_SSA_GEN (O(1)). Lookup: read slot, check generation.
No hash, no probe, no collision possible. O(1) guaranteed.
//

ALLOC_RAW BFS_QUEUE_SIZE RA_BQ_BASE

//
VS words = (RA_NEXT_ID >> 6) + 1: dynamic, covers all allocated addresses.
Same pattern as INIT_SSA_A — size from actual alloc_ptr, not hardcoded constant.
//

NOVA INIT_VS_SHR Op=Right El1=RA_NEXT_ID El2=C_6 Exit=RA_VS_WORDS
NOVA INIT_VS_INC Op=Add El1=RA_VS_WORDS El2=C_1 Exit=RA_VS_WORDS
ALLOC_RAW RA_VS_WORDS RA_VS_BASE

//
SSA direct-map: RA_NEXT_ID slots (one per allocated address).
RA_NEXT_ID = alloc_ptr at P0_NID start — exact size, no overallocation.
Generation 0 = empty slot. O(1) lookup guaranteed.
//

ALLOC_RAW RA_NEXT_ID RA_SSA_BASE
NOVA INIT_SSA_GEN Op=Move El1=C_1 Exit=RA_SSA_GEN

//
Predecessor-count array removed — was used in BFS_ENQ but never consumed elsewhere.

── INIT_RT: allocate rule-lookup table (C_1024 luces) ─
Indexed by aspect Lux address. Max derived aspect addr ~628 (Store).
C_1024 covers all current and future aspects with ample headroom.
//

ALLOC_RAW C_1024 RA_RT_BASE

//
INIT_SC_ARGS removed — no runtime table needed

INIT_RO_TABLE removed — dispatch via JEQ chain in ET_LOAD (no runtime init needed)

── INIT_OPS_JT removed: EB_OPS_BODY now uses JEQ chain on rel address ──
No runtime table needed. Rel addresses are stable (8-slot Lux, SEAL).
Dispatch: compare RA_TMP (rel address) with El1/El2/AutoNext.
//

RREDI INIT_REGS

── VS_TEST_SET: atomic test-and-set on the visited bitset ─────
//
IN:  RA_VS_ID  = Lux ID to test (caller must set before jumping)
RA_LINK    = return address
OUT: RA_FLAG   = 0 if Lux was NOT visited (bit just set for first time)
nonzero if Lux was ALREADY visited (bit was already set)
The bit is ALWAYS set after this call.
Clobbers: RA_TMP2 (lux addr), RA_TMP3 (bit mask), RA_TMP4 (word value)
//

── VS_PREPARE: compute word addr + bit mask for RA_VS_ID ──────
/OUT: RA_TMP2 = lux addr in VS array, RA_TMP3 = bit mask. Leaf.
NOVA VS_PREPARE Op=Right El1=RA_VS_ID El2=C_6 Exit=RA_TMP2 /word_idx = id >> 6
NOVA Op=And El1=RA_VS_ID El2=C_63 Exit=RA_TMP3 /bit_pos = id & 63
NOVA Op=Left El1=C_1 El2=RA_TMP3 Exit=RA_TMP3 /bit_mask
NOVA Op=Add El1=RA_VS_BASE El2=RA_TMP2 Exit=RA_TMP2 /lux addr

RREDI VSP_RET

NOLINK
RVOCA VS_TEST_SET Body=VS_PREPARE
NOVA Op=Read El1=RA_TMP2 Exit=RA_TMP4 /word value
NOVA Op=And El1=RA_TMP4 El2=RA_TMP3 Exit=RA_FLAG /test bit
NOVA Op=Or El1=RA_TMP4 El2=RA_TMP3 Exit=RA_TMP4 /set bit
NOVA Op=Write El1=RA_TMP2 El2=RA_TMP4 /write back

RREDI VTS_RET

── VS_CLEAR_BIT: clear one bit in the visited bitset ─────────
//
IN:  RA_VS_ID = Lux ID to clear. RA_LINK = return address.
OUT: (none). Clobbers: RA_TMP2 (lux addr), RA_TMP3 (bit mask), RA_TMP4 (word).
Symmetric to VS_TEST_SET. Leaf.
//

NOLINK
RVOCA VS_CLEAR_BIT Body=VS_PREPARE
NOVA Op=Xor El1=RA_TMP3 El2=C_NEG1 Exit=RA_TMP3 /inverted mask
NOVA Op=Read El1=RA_TMP2 Exit=RA_TMP4 /word value
NOVA Op=And El1=RA_TMP4 El2=RA_TMP3 Exit=RA_TMP4 /clear bit
NOVA Op=Write El1=RA_TMP2 El2=RA_TMP4 /write back

RREDI VCB_RET

── BQ_ADDR — compute circular queue lux address ─────────────
/IN:  RA_TMP = raw pointer. OUT: RA_TMP = lux address. Leaf.
NOLINK
NOVA BQ_ADDR Op=Rem El1=RA_TMP El2=BFS_QUEUE_SIZE Exit=RA_TMP
NOVA BQ_ADDR_ADD Op=Add El1=RA_BQ_BASE El2=RA_TMP Exit=RA_TMP
RREDI BQ_ADDR_RET

── VS_CLEAR — zero the visited bitset (RA_VS_WORDS luces) ─────────────────
/Leaf. Clobbers: RA_I, RA_TMP
NOLINK
CLEAR VS_CLEAR El1=RA_I
JEQ VS_CLR_LOOP El1=RA_I El2=RA_VS_WORDS Exit=VS_CLR_DONE
NOVA Op=Add El1=RA_VS_BASE El2=RA_I Exit=RA_TMP
NOVA Op=Write El1=RA_TMP El2=C_0
NOVA Op=Add El1=RA_I El2=C_1 Exit=RA_I
NOVA Op=Jump Exit=VS_CLR_LOOP

RREDI VS_CLR_DONE

//
ET_PACK_BYTE removed — byte packing is now inline in ET_COLL_W0/W1
(pointer-through-register pattern was buggy: Read of a value, not an ID)
//

── ET_PH_FRESH_ALLOC — alloc-or-reuse SSA index ──────────────
IN:  RA_FAL_REG = Lux ID of SSA register to check/assign
Non-leaf (calls EMIT_INT_ENTRY); RA_LINK saved/restored automatically.
Also saves RA_TW_LUX via SAVE macro — EMIT_INT_ENTRY/EMIT_PACKED_STR
clobber RA_TW_LUX (used as walker). SAVE restores automatically before return.
NOLINK
SAVE ET_PH_FRESH_ALLOC RA_TW_LUX
    NOVA FAL_RDVAL Op=Read El1=RA_FAL_REG Exit=RA_TMP4
    JZ FAL_CHECK El1=RA_TMP4 Exit=FAL_NEW
    /Already assigned: emit %v + existing value
    EMIT  FAL_REUSE_VPX       SF_VPX
    NOVA FAL_REUSE_INT Op=Move El1=RA_TMP4 Exit=RA_TMP2
    RCALL_AT FAL_REUSE_INTR EMIT_INT_ENTRY FAL_DONE
    /Allocate new SSA index
    NOVA FAL_NEW Op=Add El1=RA_SSA_CTR El2=C_1 Exit=RA_SSA_CTR
    NOVA FAL_SAVEPRI Op=Write El1=RA_FAL_REG El2=RA_SSA_CTR
    //
    Secondary register removed — it was corrupting native Lux words by using
    raw SSA index as Write address. Each SSA slot has its own pointer Lux.
    //
    EMIT  FAL_EMIT       SF_VPX
    NOVA FAL_SET_INT2 Op=Move El1=RA_SSA_CTR Exit=RA_TMP2
    RVOCA FAL_EMIT_INT Body=EMIT_INT_ENTRY

RREDI FAL_DONE
── ET_EMIT_SSA_REG — emit "%vN" for a given SSA register ──────
//
IN:  RA_ESAR_REG = Lux ID of the SSA index register
Non-leaf. Saves RA_TW_LUX via SAVE macro.
//
NOLINK
SAVE ET_EMIT_SSA_REG RA_TW_LUX
    NOVA ESAR_RDVAL Op=Read El1=RA_ESAR_REG Exit=RA_TMP2
    EMIT  ESAR_VPX           SF_VPX
    RVOCA ESAR_SETRET Body=EMIT_INT_ENTRY

RREDI ESAR_DONE_r

/EMIT_GEP_LINE removed - superseded by inline GEP

── P0_NID: ENTRY POINT ───────────────────────────────────────
NOVA P0_NID Op=Read El1=K_CURSOR Exit=RA_NEXT_ID
RCALL_AT P0_INIT_C INIT_BFS RT_BUILD_ENTRY

── RT_BUILD: populate rule-lookup table ─────────────────────
//
After INIT returns, scan all Lux for ForType Lumen.
For each Lux with a ForType lumen → op_id, write RT_BASE[op_id] = lux_id.
One Rule may have multiple ForType Lumen (e.g. Yaku_Read for Load+Read),
so all are registered here.
O(total_lumens) once at startup.
Use SCAN_ALL_LUX to iterate all Data Lux via LUX_REGISTRY.
Body: RTB_BODY — for each Lux, scan its ForType lumina.
//
NOVA RT_BUILD_ENTRY Op=Move El1=RTB_BODY Exit=RA_SCAN_BODY
RVOCA RTB_SCAN_J Body=SCAN_ALL_LUX
NOVA RTB_DONE Op=Jump Exit=P0_ALLOC_EPBUF

── RTB_BODY: per-Lux body called by SCAN_ALL_LUX ─────────────
//
IN: RA_I = current Lux address. Scans its Lumen for ForType lumina.
Non-leaf: saves RA_LINK, RA_SCAN_STOP, RA_SCAN_BODY (inner SLO overwrites).
RA_SCAN_BODY MUST be saved/restored: SCAN_LUMEN_OF sets it to RTB_LUX_BODY,
but outer SCAN_ALL_LUX needs it restored to RTB_BODY for next iteration.
//

NOLINK
SAVE RTB_BODY RA_SCAN_STOP RA_SCAN_BODY
    NOVA RTB_SET_LUX Op=Move El1=RA_I Exit=RA_SCAN_LUX
    NOVA RTB_SLO_BD Op=Move El1=RTB_LUX_BODY Exit=RA_SCAN_BODY
    RVOCA RTB_SLO_J Body=SCAN_LUMEN_OF

RREDI RTB_BODY_RET

── RTB_LUX_BODY: per-Lumen body for RT_BUILD ─────────────────
//
IN: RA_TW_LUMEN = current Lumen. Non-leaf (LR/LT expand to Voca).
If rel==ForType: read exit (op_id), bounds check, write RT_BASE[op_id]=RA_I.
//

NOLINK

//
RA_LINK is preserved automatically by the call stack (Voca/Redi push/pop
on RA_SP) -- no manual save/restore is ever needed, regardless of how many
early-return points a body has.
NOP, not LR, names the entry point here -- and this is NOT just an
allocation-size issue (that part IS fixed: loader.py's _scan_nova_names
prepass now discovers LR/LT/RCALL_AT/EMIT/EMITI/PUTBYTE/WALK_ONE/etc.
automatically by scanning expand.re/synth.re for the universal "Move
El1=RA_MA0 ... Exit=RA_MC_LUX" convention, instead of a hand-maintained
list -- see _discover_nova_naming_commands). The deeper reason: Python's
Wave-B (_wave2_line) natively expands only ~16 primitive commands --
NEW/NOLINK/SET/SEAL/LINK/NOVA/RVOCA/RREDI/CLEAR/NOP/
JEQ/JZ/ALLOC_TO. LR/LT/RCALL_AT/EMIT/EMITI/PUTBYTE/WALK_ONE are NOT in
that list -- they only ever get expanded by LOAD_MAIN itself, at runtime,
via generic command-word dispatch (the command's own symbol value points
at its _SPN handler; see the "Restore command handler words" block in
loader.py's freeze()). Since LOAD_MAIN doesn't yet reach yaku.re (still
blocked at file 33/wax.re), "LR RTB_LUX_BODY ..." is, right now, never
actually executed by *either* build path -- not a sizing problem, a
"nothing ever writes this content at all yet" problem. NOP is in the
~16-primitive list Python *does* natively expand, so it's correct
regardless of self-hosting progress. Confirmed by direct regression
(twice): aether[RTB_LUX_BODY] reads back as 0 with LR-direct, correct
with NOP, even after the prepass fix.
//

NOP RTB_LUX_BODY
LR RTB_LR RA_TW_LUMEN RA_TMP          /rel
JEQ RTB_FTK El1=RA_TMP El2=ForType Exit=RTB_FT
RREDI RTB_BODY_SKIP_r /not ForType → continue
LT RTB_FT RA_TW_LUMEN RA_TMP          /exit = op_id
NOVA RTB_BCHK Op=ULess El1=RA_TMP El2=C_1024 Exit=RA_TMP2 /op_id < 1024
NOVA RTB_BCHKJ Op=JumpIf El1=RA_TMP2 Exit=RTB_ADDR
RREDI RTB_OOB_RET /out of range → skip
NOVA RTB_ADDR Op=Add El1=RA_RT_BASE El2=RA_TMP Exit=RA_TMP2
NOVA RTB_WRITE Op=Write El1=RA_TMP2 El2=RA_I
RREDI RTB_WRITE_RET

── P0_COLLECT: scan LUX_REGISTRY, collect Entry luces ──────────────
//
We do this as a PURE scan (no nested scans, no EMIT_FUNC calls).
"For" each lux with Entry→Yaku or Entry→SelfYaku, store addr in EP_BUFFER.
EP_BUFFER is allocated BEFORE P0_PREAM_DONE resets K_CURSOR so it lands
above BFS/VS/SSA/RT scratch and survives the reset.
Max capacity: EP_BUF_MAX entries (32 is generous — real programs have few entries).

Allocate EP_BUFFER first, while bump is still above BFS/VS/SSA scratch.
Then emit preamble and reset K_CURSOR — EP_BUF is safely above the freed region.
//

SET EP_BUF_MAX Value=32
NEW RA_EP_BUF_BASE  /base address of EP_BUFFER
NEW RA_EP_BUF_CNT  /how many entries collected

ALLOC_RAW P0_ALLOC_EPBUF EP_BUF_MAX RA_EP_BUF_BASE
CLEAR P0_EPBUF_CLR El1=RA_EP_BUF_CNT

EMIT P0_INIT_DONE SF_DECLARE
EMIT P0_HEAPD SF_HEAPDECL
EMITI P0_HD_NUM K_AETHER_SIZE
EMIT P0_HD_XI64 SF_X_I64
PUTBYTE P0_PREAM LF
NOVA P0_PREAM_TW Op=Move El1=RA_PREAM_BASE Exit=RA_TW_LUX
RVOCA P0_PREAM2 Body=EMIT_STR_ENTRY
/Reset K_CURSOR to reclaim BFS/VS/SSA/RT scratch. EP_BUF is safely above.
NOVA P0_PREAM_DONE Op=Key El1=K_CURSOR El2=RA_NEXT_ID
CLEAR P0_INIT_FUNC_DONE El1=RA_EP_COUNT

/Collect: scan LUX_REGISTRY with P0_COLLECT_BODY (no nested scans inside body)
NOVA P0_SET_COL_BODY Op=Move El1=P0_COLLECT_BODY Exit=RA_SCAN_BODY
RVOCA P0_COLSCANJ Body=SCAN_ALL_LUX
NOVA P0_COLLECT_DONE Op=Jump Exit=P0_EMIT_INIT /SCAN_ALL_LUX returns here; jump to P0_EMIT

//
IN: RA_I = Lux address, guaranteed Data (SCAN_ALL_LUX walks LUX_REGISTRY,
the Data-only address list built by loader.py -- see accord.re: "pure data,
no type tags, no typed regions"). No type check needed or possible: type is
never a lux property, only a property of which registry you're scanning.
Check Entry->Yaku/SelfYaku via direct slot read (O(1)).
Does NOT call any other scan. Does NOT call EMIT_FUNC.
//

NOLINK
/Data lux: lumina at slot DATA_LUMEN_OFFSET. Check (rel=Entry at pos DATA_LUMEN_OFFSET)
NOVA P0_COLLECT_BODY Op=Add El1=RA_I El2=C_1 Exit=RA_TMP
NOVA P0_CB_DATA_RD Op=Read El1=RA_TMP Exit=RA_FLAG
JEQ P0_CB_DATA_EQ El1=RA_FLAG El2=Entry Exit=P0_CB_DATA_EXIT
RREDI P0_CB_SKIP_r
NOVA P0_CB_DATA_EXIT Op=Add El1=RA_I El2=C_2 Exit=RA_TMP
NOVA Op=Read El1=RA_TMP Exit=RA_TMP2
NOVA P0_CB_DATA_JMPIF Op=JumpIf El1=RA_TMP2 Exit=P0_CB_CHK_TGT
RREDI P0_CB_SKIP2_r

NOLINK
/Check exit == Yaku or SelfYaku — RA_TMP2 already holds it (read above)
JEQ P0_CB_CHK_TGT El1=RA_TMP2 El2=Yaku Exit=P0_CB_STORE
JEQ P0_CB_CHK_CL El1=RA_TMP2 El2=SelfYaku Exit=P0_CB_STORE
RREDI P0_CB_NOEP_r
NOLINK
/Store RA_I in EP_BUFFER
NOVA P0_CB_STORE Op=Add El1=RA_EP_BUF_BASE El2=RA_EP_BUF_CNT Exit=RA_TMP
NOVA P0_CB_WRITE Op=Write El1=RA_TMP El2=RA_I
NOVA P0_CB_INC Op=Add El1=RA_EP_BUF_CNT El2=C_1 Exit=RA_EP_BUF_CNT
RREDI P0_CB_RET_r

── P0_EMIT: iterate EP_BUFFER, emit one function per Entry ──────────
//
Simple index loop: RA_EP_IDX from 0 to RA_EP_BUF_CNT.
No external scan active during EMIT_FUNC — safe for nested scans.
//

CLEAR P0_EMIT_INIT El1=RA_EP_COUNT
NEW RA_EP_IDX  /current index into EP_BUFFER
CLEAR P0_IDX_CLR El1=RA_EP_IDX
JEQ P0_EMIT_LOOP El1=RA_EP_IDX El2=RA_EP_BUF_CNT Exit=P0_DONE
/Load entry addr from buffer
NOVA P0_EMIT_ADDR Op=Add El1=RA_EP_BUF_BASE El2=RA_EP_IDX Exit=RA_TMP
NOVA P0_EMIT_RD Op=Read El1=RA_TMP Exit=RA_ENTRY_LUX
/Dispatch: first entry → EMIT_FUNC (primary/@main), rest → EMIT_FUNC_SECONDARY
JZ P0_EMIT_CHK El1=RA_EP_COUNT Exit=P0_EMIT_PRIMARY
NOVA P0_EMIT_SEC Op=Add El1=RA_EP_COUNT El2=C_1 Exit=RA_EP_COUNT
RCALL_AT P0_EMIT_SECJ EMIT_FUNC_SECONDARY P0_EMIT_NEXT
NOVA P0_EMIT_PRIMARY Op=Add El1=RA_EP_COUNT El2=C_1 Exit=RA_EP_COUNT
RCALL_AT P0_EMIT_PRIMJ EMIT_FUNC P0_EMIT_NEXT
/Landing after EMIT_FUNC returns
NOVA P0_EMIT_NEXT Op=Add El1=RA_EP_IDX El2=C_1 Exit=RA_EP_IDX
NOVA P0_EMIT_LB Op=Jump Exit=P0_EMIT_LOOP

RVOCA P0_DONE Body=FLUSH
NOVA P0_HALT Op=End

//
P0_PREAM_TW: RA_TW_LUX must receive the ID of PREAM_0000, not its word.
Move El1=PREAM_0000 reads aether[PREAM_0000] = packed bytes (word) — wrong.
SEAL Name=RA_PREAM_BASE PREAM_0000 at freeze sets aether[RA_PREAM_BASE]=PREAM_0000_id.
Then Read RA_PREAM_BASE → RA_TW_LUX gives the correct ID.
//

SET RA_PREAM_BASE Value=PREAM_0000
/EMIT_STR_ENTRY in output.re already decodes packed u64 byte chains identically.
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
NOVA EF_TO_BODY Op=Jump Exit=EF_VSCLEAR

//
Secondary entry: full function — header only differs from primary (no @reca_init).
LLVM labels are function-local; secondary cannot branch into @main's labels.
Solution: secondary runs its own EMIT_FUNC_BODY (VS clear, BFS from own entry, etc.).
//

NOLINK
EMIT EMIT_FUNC_SECONDARY SF_FUNCOPEN
EMIT EF_SEC_L SF_FUNCSEC
EMITI EF_SEC_ID RA_ENTRY_LUX
EMIT EF_SEC_BODY SF_FUNCSECBODY
EMITI EF_SEC_BRL RA_ENTRY_LUX
PUTBYTE EF_SEC_NL LF
NOVA EF_SEC_TO_BODY Op=Jump Exit=EF_VSCLEAR

/Common body: VS clear, BFS, EMIT_MISSED, JR_DISPATCH — runs ONCE for primary only.
NOLINK
/I.13: replaced inline VS clear loop with VS_CLEAR subroutine.
RVOCA EF_VSCLEAR Body=VS_CLEAR
CLEAR EF_VSCLEAR_DONE El1=RA_SSA_CTR
/Invalidate SSA cache at function boundary — new generation, all slots stale.
NOVA EF_SSA_GEN_INC Op=Add El1=RA_SSA_GEN El2=C_1 Exit=RA_SSA_GEN
CLEAR EF_BQRST El1=RA_BQ_HEAD
CLEAR EF_BQRST2 El1=RA_BQ_TAIL
NOVA EF_ENQUEUE Op=Move El1=RA_ENTRY_LUX Exit=RA_TMP
RVOCA EF_ENQ_R Body=BFS_ENQ
RVOCA EF_BFSJ Body=BFS_LOOP_SAVE
RCALL_AT EF_BFS_DONE EMIT_MISSED EF_EMISS
RCALL_AT EF_EMISS EMIT_JR_DISPATCH EF_EJRD
EMIT EF_EJRD SF_FUNCEND
NOVA EF_RESTORE_SCAN Op=Move El1=RA_ENTRY_LUX Exit=RA_SCAN_LUX /restore RA_SCAN_LUX clobbered by RTB/BFS_SCAN_LUX inside EMIT_FUNC
RREDI EF_RET_r

── EMIT_MISSED: emit basic blocks not reached by BFS ──────────
//
Uses SCAN_NOVA_LUX (instruction-only registry) so RO/Data Lux
are not accidentally compiled as instruction blocks.
//

NOVA EMIT_MISSED Op=Move El1=EM_MISSED_BODY Exit=RA_SCAN_BODY
RVOCA EM_SCANJ Body=SCAN_NOVA_LUX
RREDI EM_DONE_r

── EM_MISSED_BODY: per-Lux body for EMIT_MISSED ───────────────
//
IN: RA_I = Lux address. Non-leaf (WALK_ONE + VS_TEST_SET + RCALL_AT).
Saves RA_SCAN_BODY, RA_SCAN_STOP, RA_SAL_CUR, SAL_REG_END so EMIT_BLOCK
doesn't clobber them. RA_LINK is preserved automatically by the call stack
(Voca/Redi push/pop on RA_SP) -- removed from the manual SAVE list; it was
never a valid manual-save target in the first place.
//

NOLINK
NORESTORE
SAVE EM_MISSED_BODY RA_SCAN_BODY RA_SCAN_STOP RA_SAL_CUR SAL_REG_END
    NOVA EM_SET_I Op=Move El1=RA_I Exit=RA_EM_I
    /Check Op: read directly from SLOT_OP (compact NOVA slot 1)
    NOVA EM_OPCHK Op=Add El1=RA_EM_I El2=C_1 Exit=RA_TMP
    NOVA EM_OPDRD Op=Read El1=RA_TMP Exit=RA_FLAG
    NOVA EM_OPSRDJ Op=JumpIf El1=RA_FLAG Exit=EM_EPRD7
    NOVA EM_OPNOOP Op=Jump Exit=EM_NOOP
    NOLINK
    //
    Has Op: fast O(1) Entry check — read slot 6 (pad; doubles as the
    first extra lumen's own rel when one exists).
    NOVA luxs with LINK X Entry Yaku store (Entry_addr, Loom_addr) at
    slots 6,7 -- extra lumina start right at the pad slot itself now,
    not after it (see constants.re's NOVA layout notes and
    interpreter.py's _nova_extra_skip).
    SelfYaku entries also have Entry at slot 6 — now also skipped (separate function).
    //
    NOLINK
    NOVA EM_EPRD7 Op=Add El1=RA_EM_I El2=C_6 Exit=RA_TMP /slot 6 = first extra lumen rel
    NOVA EM_EPRDREL Op=Read El1=RA_TMP Exit=RA_FLAG /read rel
    JEQ EM_EPRELEQ El1=RA_FLAG El2=Entry Exit=EM_EPTGT
    NOVA EM_EPNO_ENT Op=Jump Exit=EM_HASOP /no Entry lumen → compile
    NOLINK
    /Has Entry lumen: read exit at slot 7
    NOVA EM_EPEXIT Op=Add El1=RA_EM_I El2=C_7 Exit=RA_TMP /slot 7 = exit
    NOVA EM_EPRDEXIT Op=Read El1=RA_TMP Exit=RA_FLAG /read exit
    JEQ EM_EPLM_CK El1=RA_FLAG El2=Yaku Exit=EM_NOOP
    JEQ EM_EPCLM_CK El1=RA_FLAG El2=SelfYaku Exit=EM_NOOP
    /Other Entry target → fall through to compile
    NOVA EM_EPNO_LOOM Op=Jump Exit=EM_HASOP
    NOVA EM_HASOP Op=Move El1=RA_EM_I Exit=RA_VS_ID
    RVOCA EM_VTSR Body=VS_TEST_SET
    NOVA EM_VTS_CHK Op=JumpIf El1=RA_FLAG Exit=EM_VISITED
    NOVA EM_EMIT Op=Move El1=RA_EM_I Exit=RA_INSTR
    //
    Set RA_EB_OUTER=EM_VISITED: EMIT_BLOCK returns here, not to BFS_SCAN_LUX.
    EB_SAVE_RET was wrong: it set RA_EB_OUTER=BFS_SCAN_LUX causing BFS restart per block.
    //
    RCALL_AT EM_EB_OUTER EMIT_BLOCK EM_VISITED
    /Shared restore path for NOOP and VISITED — identical register restores
    NOVA EM_NOOP Op=Move El1=S_RA_SCAN_BODY Exit=RA_SCAN_BODY
    NOVA EM_VISITED Op=Move El1=S_RA_SCAN_STOP Exit=RA_SCAN_STOP
    NOVA EM_RST_RSC Op=Move El1=S_RA_SAL_CUR Exit=RA_SAL_CUR
    NOVA EM_RST_RSRE Op=Move El1=S_SAL_REG_END Exit=SAL_REG_END
    RREDI EM_RST_r

── EMIT_JR_DISPATCH: emit %L_jrdispatch switch block ──────────
EMIT EMIT_JR_DISPATCH SF_JR_HEAD
EMIT EJR_LOAD SF_JR_LOAD
CLEAR EJR_SCAN El1=RA_I
JEQ EJR_WLOOP El1=RA_I El2=RA_VS_WORDS Exit=EJR_TAIL
NOVA EJR_WADDR Op=Add El1=RA_VS_BASE El2=RA_I Exit=RA_TMP
NOVA EJR_WLOAD Op=Read El1=RA_TMP Exit=RA_JR_WORD
JZ EJR_WNZ El1=RA_JR_WORD Exit=EJR_WNXT
CLEAR EJR_BLOOP El1=RA_JR_BI
JEQ EJR_BLOOPCK El1=RA_JR_BI El2=C_64 Exit=EJR_WNXT
NOVA EJR_BMASK Op=Left El1=C_1 El2=RA_JR_BI Exit=RA_TMP2
NOVA EJR_BTEST Op=And El1=RA_JR_WORD El2=RA_TMP2 Exit=RA_TMP2
JZ EJR_BCHK El1=RA_TMP2 Exit=EJR_BNXT
NOVA EJR_NCOMP Op=Left El1=RA_I El2=C_6 Exit=RA_JR_LID /word_idx * 64 = word_idx << 6
NOVA EJR_NADD Op=Add El1=RA_JR_LID El2=RA_JR_BI Exit=RA_JR_LID
EMIT EJR_ARMPFX SF_JR_ARM_PFX
EMITI EJR_ARMNID RA_JR_LID
EMIT EJR_ARMMID SF_JR_ARM_MID
EMITI EJR_ARMLNID RA_JR_LID
PUTBYTE EJR_ARMLNL LF
NOVA EJR_BNXT Op=Add El1=RA_JR_BI El2=C_1 Exit=RA_JR_BI
NOVA EJR_BNXTL Op=Jump Exit=EJR_BLOOPCK
NOVA EJR_WNXT Op=Add El1=RA_I El2=C_1 Exit=RA_I
NOVA EJR_WNXTL Op=Jump Exit=EJR_WLOOP
EMIT EJR_TAIL SF_JR_TAIL
RREDI EJR_RET_r

── BFS_ENQ: enqueue RA_TMP if not visited ────────────────────
//
Non-leaf — RA_LINK is saved/restored automatically by the call stack.
Guard: only enqueue NOVA luxs (Op != 0). Data luces have Op=0 → undefined labels in JR_DISPATCH.
//

NOVA BFS_ENQ Op=Add El1=RA_TMP El2=C_1 Exit=RA_TMP2 /addr+SLOT_OP
NOVA BEN_OPRD Op=Read El1=RA_TMP2 Exit=RA_TMP2 /read Op
NOVA BEN_OPTEST Op=JumpIf El1=RA_TMP2 Exit=BEN_IS_NOVA
RREDI BEN_SKIP_r
NOVA BEN_IS_NOVA Op=Move El1=RA_TMP Exit=RA_VS_ID
RVOCA BEN_VTR Body=VS_TEST_SET
NOVA Op=JumpIf El1=RA_FLAG Exit=BEN_ALREADY
NOVA Op=Move El1=RA_BQ_TAIL Exit=RA_TMP
RVOCA BEN_BQARJ Body=BQ_ADDR
NOVA Op=Write El1=RA_TMP El2=RA_VS_ID
NOVA Op=Add El1=RA_BQ_TAIL El2=C_1 Exit=RA_BQ_TAIL
RREDI BEN_RETJ

RREDI BEN_ALREADY
── BFS_LOOP: dequeue + emit block + enqueue successors ───────
/RA_LINK is saved/restored automatically by the call stack./
SET BFS_LOOP_SAVE Value=BFS_LOOP /alias: external entry point/
JEQ BFS_LOOP El1=RA_BQ_HEAD El2=RA_BQ_TAIL Exit=BFS_DONE
NOVA BFS_DEQR Op=Move El1=RA_BQ_HEAD Exit=RA_TMP
RVOCA BFS_DEQBQRJ Body=BQ_ADDR
NOVA BFS_DEQDONE Op=Read El1=RA_TMP Exit=RA_INSTR
NOVA BFS_HINC Op=Add El1=RA_BQ_HEAD El2=C_1 Exit=RA_BQ_HEAD
RCALL_AT EB_SAVE_RET EMIT_BLOCK BFS_SCAN_LUX

//
Enqueue successors: read SLOT_NEXT directly + check SLOT_EXIT for Jump/JumpIf.
Only Jump and JumpIf have Exit = static NOVA address → must follow for correct CFG.
Voca/JumpReg/Redi Exit is runtime (RA_LINK or register) → must NOT follow.
//

NOVA BFS_SCAN_LUX Op=Add El1=RA_INSTR El2=C_5 Exit=RA_TMP /SLOT_NEXT=5
NOVA BFS_RD_NXT Op=Read El1=RA_TMP Exit=RA_TMP /read next NOVA addr
JZ BFS_NXTCHK El1=RA_TMP Exit=BFS_SCAN_EXIT /0 (NOLINK) → skip to exit check
/RA_TMP already holds SLOT_NEXT addr — enqueue directly
RCALL_AT BFS_ENQ_NJ BFS_ENQ BFS_SCAN_EXIT
//
Check SLOT_EXIT for Jump / JumpIf, and SLOT_E1 for Voca.
Jump/JumpIf: Exit = static destination block address.
Voca: El1 = static target subroutine address (Exit=RA_LINK is dynamic, skip it).
Redi/JumpReg: fully dynamic → skip.
//

NOVA BFS_SCAN_EXIT Op=Add El1=RA_INSTR El2=C_1 Exit=RA_TMP2 /SLOT_OP=1
NOVA BFS_EXIT_ROP Op=Read El1=RA_TMP2 Exit=RA_TMP2 /read op_id

SWITCH RA_TMP2
    Jump JumpIf > BFS_EXIT_DO
    Voca        BFS_VOCA_DO

/Not a CFG-branching op → back to BFS_LOOP
NOVA BFS_EXIT_SKIP Op=Jump Exit=BFS_LOOP
NOLINK
/Jump/JumpIf: enqueue SLOT_EXIT (slot 4)
NOVA BFS_EXIT_DO Op=Add El1=RA_INSTR El2=C_4 Exit=RA_TMP /SLOT_EXIT=4
NOVA BFS_EXIT_RD Op=Read El1=RA_TMP Exit=RA_TMP /read exit target addr
JZ BFS_EXIT_NZ El1=RA_TMP Exit=BFS_LOOP /0 → skip
RCALL_AT BFS_EXIT_EJ BFS_ENQ BFS_LOOP
/Voca: enqueue SLOT_E1 (slot 2) — the static call target
NOVA BFS_VOCA_DO Op=Add El1=RA_INSTR El2=C_2 Exit=RA_TMP /SLOT_E1=2
NOVA BFS_VOCA_RD Op=Read El1=RA_TMP Exit=RA_TMP /read e1 = target addr
JZ BFS_VOCA_NZ El1=RA_TMP Exit=BFS_LOOP /0 → skip
RCALL_AT BFS_VOCA_EJ BFS_ENQ BFS_LOOP
/BFS_DONE: queue empty — return (automatic stack restores RA_LINK).
RREDI BFS_DONE
── EMIT_BLOCK: emit one basic block for RA_INSTR ─────────────
/Check Op via direct SLOT_OP read (compact NOVA slot 1, not a lumen)
NOVA EMIT_BLOCK Op=Add El1=RA_INSTR El2=C_1 Exit=RA_TMP
NOVA EB_RDOP Op=Read El1=RA_TMP Exit=RA_FLAG
NOVA EB_PREOPDJ Op=JumpIf El1=RA_FLAG Exit=EB_EMIT_LBL

//
Lux without Op: not an instruction, skip without clearing visited bit.
(Clearing the bit caused infinite BFS loops — a Lux can be re-enqueued
after its bit is cleared, leading EMIT_BLOCK to clear it again, repeat.)
//

RREDI EB_EARLY_RET_r

PUTBYTE EB_EMIT_LBL LF
PUTBYTE EB_LBL_L ASCII_L
EMITI EB_LNID RA_INSTR
PUTBYTE EB_LCOL COLON
PUTBYTE EB_LNL LF
/Read Op directly from SLOT_OP (slot 1). At this point Op is known non-zero.
NOVA EB_FINDOP Op=Add El1=RA_INSTR El2=C_1 Exit=RA_TMP
NOVA EB_FD_RD Op=Read El1=RA_TMP Exit=RA_OP_ID
CLEAR EB_NO_OP El1=RA_A1
CLEAR EB_CLR2 El1=RA_A2
CLEAR EB_CLR3 El1=RA_EXIT_N
CLEAR EB_CLR5 El1=RA_NXT_N
FOR RA_SSA_FR RA_SSA_FR2 RA_SSA_RESULT RA_SSA_V1 RA_SSA_V2
    NOVA EB_CLR_{N} Op=Write El1={X} El2=C_0 /zero SSA slot

/SSA cache: always invalidated per BB (JR-dispatch makes every BB a potential entry).
NOVA EB_SSA_GEN_INC Op=Add El1=RA_SSA_GEN El2=C_1 Exit=RA_SSA_GEN
CLEAR EB_SSA_GEN_DONE El1=RA_TMP

//
Read El1/El2/Exit/Next from compact NOVA fixed slots (O(1) direct reads).
RA_EXIT_N = SLOT_EXIT: exit is unified — for jumps it is destination block,
for arithmetic/move it is target register. {ldest} (ETH_LDEST_SV) only appears in jump templates.
//

NOVA Op=Add El1=RA_INSTR El2=C_2 Exit=RA_TMP
NOVA Op=Read El1=RA_TMP Exit=RA_A1
NOVA Op=Add El1=RA_INSTR El2=C_3 Exit=RA_TMP
NOVA Op=Read El1=RA_TMP Exit=RA_A2
NOVA Op=Add El1=RA_INSTR El2=C_4 Exit=RA_TMP
NOVA Op=Read El1=RA_TMP Exit=RA_EXIT_N
NOVA Op=Add El1=RA_INSTR El2=C_5 Exit=RA_TMP
NOVA Op=Read El1=RA_TMP Exit=RA_NXT_N

── Equal+JumpIf fusion check ────────────────────────────────────────────────
//
If op==JumpIf AND RA_SSA_CMP!=0 AND RA_A1==RA_JEQ_FLAG:
  use JumpIfCmp rule (direct br i1 {cmp} without load+icmp ne)
If op==JumpIf AND RA_SSA_ARITH_TGT!=0 AND RA_A1==RA_SSA_ARITH_TGT:
  use JumpIfNZ rule (icmp ne {arith}, 0 + br)
Otherwise: clear pending state
//

JEQ EB_OPJIFCK El1=RA_OP_ID El2=JumpIf Exit=EB_FUSE_ISJI
/Not JumpIf: clear pending cmp and arith state
CLEAR EB_CLR_CMP El1=RA_SSA_CMP
CLEAR EB_CLR_AI El1=RA_SSA_ARITH_IDX
CLEAR EB_CLR_AT El1=RA_SSA_ARITH_TGT
NOVA EB_NOFUSE Op=Jump Exit=EB_FINDTMPL
NOLINK
JZ EB_FUSE_CMPCZ El1=RA_SSA_CMP Exit=EB_FUSE_ARITH /no pending cmp → try arith
JEQ EB_FUSE_A1CK El1=RA_A1 El2=RA_JEQ_FLAG Exit=EB_FUSE_DO_CMP
NOVA EB_FUSE_NOTA1 Op=Jump Exit=EB_FUSE_ARITH
NOLINK
NOVA EB_FUSE_DO_CMP Op=Add El1=RA_RT_BASE El2=JumpIfCmp Exit=RA_TMP
NOVA EB_FUSE_RD_CMP Op=Read El1=RA_TMP Exit=RA_RULE
NOVA EB_FUSE_GO_CMP Op=Jump Exit=EB_DO_EMIT
/Try arith fusion
NOLINK
JZ EB_FUSE_ARITH El1=RA_SSA_ARITH_TGT Exit=EB_FINDTMPL
JEQ EB_FUSE_A1AT El1=RA_A1 El2=RA_SSA_ARITH_TGT Exit=EB_FUSE_DO_NZ
NOVA EB_FUSE_NOAT Op=Jump Exit=EB_FINDTMPL
NOLINK
NOVA EB_FUSE_DO Op=Add El1=RA_RT_BASE El2=JumpIfNZ Exit=RA_TMP
NOVA EB_FUSE_RD Op=Read El1=RA_TMP Exit=RA_RULE
NOVA EB_FUSE_DO_NZ Op=Jump Exit=EB_DO_EMIT

── EB_FINDTMPL: O(1) rule lookup via pre-built rule table ────
NOVA EB_FINDTMPL Op=Add El1=RA_RT_BASE El2=RA_OP_ID Exit=RA_TMP
NOVA EB_FT_RD Op=Read El1=RA_TMP Exit=RA_RULE

//
auto-Next falls through to JZ EB_EMIT_TMPL (EB_FT_FOUND Jump was redundant)
EB_DO_EMIT: entry point when RA_RULE is already set (e.g. fusion path)
//

JZ EB_DO_EMIT El1=RA_RULE Exit=EB_FALL
RVOCA EB_PRELOADS Body=EMIT_PRELOADS
NOVA EB_EMIT_TMPL_DONE Op=Read El1=RA_RULE Exit=RA_TW_LUX /Rule.word = first byte Lux
RVOCA EB_ETMPLR Body=EMIT_TMPL
/After emit: if rule HasCmpResult → save RA_SSA_FR → RA_SSA_CMP for fusion with next JumpIf
WALK_ONE EB_HASCMP RA_RULE HasCmpResult
JZ EB_HASCMP_CHK El1=RA_SR_OUT Exit=EB_HASARITH /no HasCmpResult → check arith
/Has pending icmp i1: save RA_SSA_FR (fresh SSA index) into RA_SSA_CMP
NOVA EB_SAVE_CMP Op=Read El1=RA_SSA_FR Exit=RA_SSA_CMP
/Clear arith state — cmp replaces arith
CLEAR EB_CLR_ARITH El1=RA_SSA_ARITH_IDX
CLEAR EB_CLR_ARTGT El1=RA_SSA_ARITH_TGT
NOVA EB_POST_CMP Op=Jump Exit=EB_FALL /continue to Terminates check
/Check HasArithResult: save RA_SSA_RESULT + RA_EXIT_N as pending arith for JumpIfNZ
NOLINK
WALK_ONE EB_HASARITH RA_RULE HasArithResult
JZ EB_HASARITH_CHK El1=RA_SR_OUT Exit=EB_FALL /no HasArithResult → continue
NOVA EB_SAVE_ARITH Op=Read El1=RA_SSA_RESULT Exit=RA_SSA_ARITH_IDX
NOVA EB_SAVE_ARTGT Op=Move El1=RA_EXIT_N Exit=RA_SSA_ARITH_TGT
/Clear cmp state — arith replaces cmp
CLEAR EB_CLR_CMPF El1=RA_SSA_CMP
NOVA EB_POST_ARITH Op=Jump Exit=EB_FALL /continue to Terminates check
NOLINK
WALK_ONE EB_FALL RA_RULE Terminates
JZ EB_FALL_DN El1=RA_SR_OUT Exit=EB_DO_FALL

RREDI EB_NO_FALL_r /has Terminates → no fallthrough
JZ EB_DO_FALL El1=RA_NXT_N Exit=EB_EMIT_UNREACH
EMIT EB_BRBR SF_BR
EMITI EB_BRNID RA_NXT_N
PUTBYTE EB_BRNL LF
EMIT EB_EMIT_UNREACH SF_UNREACHABLE

//
(EB_NO_FALL moved above; EB_EMIT_UNREACH falls through to EB_NO_FALL if needed)
Second JumpReg for the fallthrough path after emitting unreachable/br:
//

RREDI EB_FALL_END_r

── EMIT_PRELOADS: emit GEP+load for each operand ─────────────
JZ EMIT_PRELOADS El1=RA_A1 Exit=EPL_A2
NOVA EPL_A1 Op=Move El1=RA_A1 Exit=RA_TMP
RVOCA EPL_A1R Body=PRELOAD_ARG
NOVA EPL_A1SAVE Op=Write El1=RA_SSA_V1 El2=RA_SSA_OUT /write ssa_idx into DATA slot, preserve pointer
JZ EPL_A2 El1=RA_A2 Exit=EPL_EXIT
NOVA EPL_A2B Op=Move El1=RA_A2 Exit=RA_TMP
RVOCA EPL_A2R Body=PRELOAD_ARG
NOVA EPL_A2SAVE Op=Write El1=RA_SSA_V2 El2=RA_SSA_OUT /write ssa_idx into DATA slot, preserve pointer
JZ EPL_EXIT El1=RA_EXIT_N Exit=EPL_SYSCHK

//
Target: inline GEP — no separate %ptN. ETH_PTR_TGT emits getelementptr inbounds inline.
RA_EXIT_N already set by EB_STEXIT. Nothing to emit here.
//

JEQ EPL_SYSCHK El1=RA_OP_ID El2=Exire Exit=EPL_SYS_A0
NOVA EPL_NOSYS Op=Jump Exit=EPL_DONE

//
Exire preload: 5 explicit PRELOAD_ARG calls for SC_A0..SC_A3, SC_NR.
Use Idem (Aspect #25) directly so RA_TMP = SC_*'s own Lux ID,
not heap[SC_*] (runtime value which changes after Move SC_* writes into it).
//

FOR A0 A1 A2 A3 NR
    NOVA EPL_SYS_{X} Op=Idem El1=SC_{X} Exit=RA_TMP
    RVOCA EPL_SYS_{X}R Body=PRELOAD_ARG

RREDI EPL_DONE
── PRELOAD_ARG: emit inline-GEP load for one operand Lux ──────────
//
Emits: "  %vN = load i64, ptr getelementptr inbounds ([S x i64], ptr @heap, i64 0, i64 <lid>)"
No separate %ptN register — GEP is inlined into load operand (valid LLVM IR).
//

NOLINK
NOVA PRELOAD_ARG Op=Move El1=RA_TMP Exit=RA_SSA_LID
NOVA PA_SAVNID Op=Move El1=RA_TMP Exit=RA_PA_LID
RCALL_AT PA_GS GET_SSA PA_ISNEW
JZ PA_ISNEW El1=RA_SSA_HIT Exit=PA_EMIT_ILOAD
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
RREDI PA_RET_r

── GET_SSA: lookup or assign SSA index for Lux RA_SSA_LID ──
//
Direct-map: slot = RA_SSA_BASE + lux_id. Format: (gen << 32) | ssa_idx.
Hit: slot_gen == RA_SSA_GEN. Miss: allocate new SSA index, write slot.
Leaf (no sub-calls → no OUTER needed).
IN:  RA_SSA_LID = Lux ID to look up
OUT: RA_SSA_OUT = SSA index (existing or newly assigned)
RA_SSA_HIT = 0 if new (slot was miss), nonzero if found
//

NOLINK
NOVA GET_SSA Op=Add El1=RA_SSA_BASE El2=RA_SSA_LID Exit=RA_TMP
NOVA GS_LOAD Op=Read El1=RA_TMP Exit=RA_TMP2
NOVA GS_EXTGEN Op=Right El1=RA_TMP2 El2=C_32 Exit=RA_TMP3
NOVA GS_HITEQ Op=Equal El1=RA_TMP3 El2=RA_SSA_GEN Exit=RA_SSA_HIT
JZ GS_HITCHK El1=RA_SSA_HIT Exit=GS_NEW
/Hit: extract ssa_idx from lower 32 bits
NOVA GS_FOUND Op=And El1=RA_TMP2 El2=MASK_LOW32 Exit=RA_SSA_OUT
RREDI GS_FRET
/Miss: allocate new SSA index, write slot
NOVA GS_NEW Op=Add El1=RA_SSA_CTR El2=C_1 Exit=RA_SSA_CTR
NOVA GS_NASSIGN Op=Move El1=RA_SSA_CTR Exit=RA_SSA_OUT
NOVA GS_PACKGEN Op=Left El1=RA_SSA_GEN El2=C_32 Exit=RA_TMP2
NOVA GS_PACKIDX Op=Or El1=RA_TMP2 El2=RA_SSA_CTR Exit=RA_TMP2
NOVA GS_NSTORE Op=Write El1=RA_TMP El2=RA_TMP2
RREDI GS_NRET

── EMIT_TMPL: read packed-string template byte by byte ──
//
IN: RA_TW_LUX = first lux of packed template (Rule.word).
Non-leaf — RA_LINK is saved/restored automatically by the call stack.
Packed string: bytes packed little-endian into u64 luces, stride 1 (see
loader.py _alloc_raw_block(1), "matches Reca").
Escape bytes 0x01-0x1F = placeholder dispatch. 0x0A = newline. 0x20+ = literal ASCII.
Null byte (0x00) terminates.
//

SET EMIT_TMPL Value=ET_LOAD /alias/

── Load next lux ─────────────────────────────────────────────────────────
NOLINK
NOVA ET_LOAD Op=Read El1=RA_TW_LUX Exit=RA_ET_WORD /word = aether[lux]
JZ ET_LOADCHK El1=RA_ET_WORD Exit=ET_DONE /word==0 → done
CLEAR ET_SHIFTINIT El1=RA_ET_SHIFT /shift = 0

── Extract one byte from current word ─────────────────────────────────────
NOVA ET_EXTRACT Op=Right El1=RA_ET_WORD El2=RA_ET_SHIFT Exit=RA_ET_BYTE
NOVA ET_MASKBYTE Op=And El1=RA_ET_BYTE El2=C_255 Exit=RA_ET_BYTE
JZ ET_BYTECHECK El1=RA_ET_BYTE Exit=ET_DONE /byte==0 → null terminator

/Dispatch: byte >= 0x20 → literal, byte < 0x20 → escape
JEQ ET_IS20 El1=RA_ET_BYTE El2=C_32 Exit=ET_LITERAL /== 0x20 → literal (space)
NOVA ET_LTCHECK Op=Right El1=RA_ET_BYTE El2=C_5 Exit=RA_TMP /byte >> 5
JZ ET_ESCCHECK El1=RA_TMP Exit=ET_ESCAPE />> 5 == 0 → byte < 0x20

── Literal byte (0x20..0xFF): emit directly ────────────────────────────────
NOVA ET_LITERAL Op=Move El1=RA_ET_BYTE Exit=RA_BYTE
RVOCA ET_LIT_PUT Body=PUT_BYTE
NOVA ET_LIT_JN Op=Jump Exit=ET_ADVANCE
NOLINK
NOVA Op=Jump Exit=ET_ADVANCE_FIRST
NOLINK
NOVA ET_ESCAPE Op=Jump Exit=ET_ADVANCE

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

NOVA ET_ADVANCE Op=Add El1=RA_ET_SHIFT El2=C_8 Exit=RA_ET_SHIFT
JEQ ET_LUXCHK El1=RA_ET_SHIFT El2=C_64 Exit=ET_NEXT_LUX
NOVA ET_LOOPBACK Op=Jump Exit=ET_EXTRACT
NOLINK
NOVA ET_NEXT_LUX Op=Add El1=RA_TW_LUX El2=C_2 Exit=RA_TW_LUX
NOVA ET_LUXLOOP Op=Jump Exit=ET_LOAD
NOLINK
NOVA ET_ADVANCE_FIRST Op=Jump Exit=ET_ADVANCE

── ETH_* handlers ──────────────────────────────────────────────────────────
/Each handler emits its value then jumps to ET_ADVANCE to consume the escape byte.

NOLINK
NOVA ETH_V1 Op=Move El1=RA_SSA_V1 Exit=RA_ESAR_REG
RVOCA ETH_V1J Body=ET_EMIT_SSA_REG
NOVA ETH_V1_JN Op=Jump Exit=ET_ADVANCE
NOLINK
NOVA ETH_V2 Op=Move El1=RA_SSA_V2 Exit=RA_ESAR_REG
RVOCA ETH_V2J Body=ET_EMIT_SSA_REG
NOVA ETH_V2_JN Op=Jump Exit=ET_ADVANCE
NOLINK
NOVA ETH_RESULT Op=Move El1=RA_SSA_RESULT Exit=RA_FAL_REG
RVOCA ETH_RES_J Body=ET_PH_FRESH_ALLOC
NOVA ETH_RES_JN Op=Jump Exit=ET_ADVANCE
NOLINK
NOVA ETH_FRESH Op=Move El1=RA_SSA_FR Exit=RA_FAL_REG
RVOCA ETH_FR_J Body=ET_PH_FRESH_ALLOC
NOVA ETH_FR_JN Op=Jump Exit=ET_ADVANCE
NOLINK
NOVA ETH_FRESH2 Op=Move El1=RA_SSA_FR2 Exit=RA_FAL_REG
RVOCA ETH_FR2_J Body=ET_PH_FRESH_ALLOC
NOVA ETH_FR2_JN Op=Jump Exit=ET_ADVANCE
NOLINK
NOVA ETH_CMP Op=Move El1=RA_SSA_CMP Exit=RA_ESAR_REG
RVOCA ETH_CMP_J Body=ET_EMIT_SSA_REG
NOVA ETH_CMP_JN Op=Jump Exit=ET_ADVANCE

NOLINK
NOVA ETH_ARITH Op=Move El1=RA_SSA_ARITH_IDX Exit=RA_ESAR_REG
RVOCA ETH_ARITH_J Body=ET_EMIT_SSA_REG
NOVA ETH_ARITH_JN Op=Jump Exit=ET_ADVANCE
NOLINK
NOVA ETH_PTR_TGT_SV Op=Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
EMIT  ETH_PTR_TGT  SF_PTR_IGEP
EMITI ETH_PT_SZ    K_AETHER_SIZE
EMIT  ETH_PT_MID   SF_GEP_MID
EMITI ETH_PT_I     RA_EXIT_N
EMIT  ETH_PT_CL    SF_IGEP_CLOSE
NOVA ETH_PT_JN Op=Jump Exit=ETH_TW_RESTORE
NOLINK
NOVA ETH_LDEST_SV Op=Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
EMIT  ETH_LDEST    SF_LBL
EMITI ETH_LD_I     RA_EXIT_N
NOVA ETH_LD_JN Op=Jump Exit=ETH_TW_RESTORE
NOLINK
NOVA ETH_LNEXT_SV Op=Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
EMIT  ETH_LNEXT    SF_LBL
EMITI ETH_LN_I     RA_NXT_N
NOVA ETH_LN_JN Op=Jump Exit=ETH_TW_RESTORE
NOLINK
NOVA ETH_LNEXT_ID_SV Op=Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
EMITI ETH_LN_ID_I  RA_NXT_N
NOVA ETH_LN_ID_JN Op=Jump Exit=ETH_TW_RESTORE
NOLINK
NOVA ETH_XLEN_SV Op=Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
EMITI ETH_XLEN     K_XLEN
NOVA ETH_XL_JN Op=Jump Exit=ETH_TW_RESTORE
NOLINK
NOVA ETH_TOTALSZ_SV Op=Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
EMITI ETH_TOTALSZ  K_AETHER_SIZE
NOVA ETH_TS_JN Op=Jump Exit=ETH_TW_RESTORE
NOLINK
NOVA ETH_FLB_SV Op=Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
EMITI ETH_FLB      C_4
NOVA ETH_FL_JN Op=Jump Exit=ETH_TW_RESTORE

/ETH_VX0..ETH_VX8: emit %v + GET_SSA(SC_Ax)
FOR SC_A0 SC_A1 SC_A2 SC_A3 SC_NR
    NOVA ETH_VX{N} Op=Idem El1={X} Exit=RA_SSA_LID
    RCALL_AT ETH_VX{N}J  GET_SSA ETH_VX_SAVE

NOVA ETH_VX_SAVE Op=Move El1=RA_TW_LUX Exit=RA_ETH_TW_SAVED
EMIT  ETH_VX_EMIT  SF_VPX
EMITI ETH_VX_I     RA_SSA_OUT
NOVA ETH_VX_JN Op=Jump Exit=ETH_TW_RESTORE

── ETH_TW_RESTORE: shared restore+advance for ETH_* save-pattern handlers ──
//
IN:  RA_ETH_TW_SAVED = previously saved RA_TW_LUX value.
Restores RA_TW_LUX then continues at ET_ADVANCE.
//

NOLINK
NOVA ETH_TW_RESTORE Op=Move El1=RA_ETH_TW_SAVED Exit=RA_TW_LUX
NOVA ETH_TW_JN Op=Jump Exit=ET_ADVANCE

RREDI ET_DONE
── RETURN ADDRESS SELF-REFERENCES ────────────────────────────

── ENTRY POINT LINK ──────────────────────────────────────────
//
P0_NID is the compiler NOVA entry. SelfYaku prevents P0_SCAN
(which checks Entry→Yaku, not Entry→SelfYaku) from compiling
the compiler itself. NOVA luxs can carry extra LINK lumen safely —
interpreter scans for rel=Next, not pc+fixed_size.
//

LINK P0_NID Rel=Entry Exit=SelfYaku

/RT_BUILD_ENTRY is the RCALL_AT landing for P0_INIT_C → INIT_BFS.
SET SelfYaku
This explicit Next link ensures BFS reaches it statically (NOLINK breaks auto-Next).
LINK P0_INIT_C Rel=Next Exit=RT_BUILD_ENTRY
── Stubs for unimplemented functions ─────────────────────────
//
These are entry points of functions not yet written.
Declared as NEW to silence Wave-B Unknown name errors.
Will be replaced with real implementations.
EM_NOOP is now a real NOVA lux (merged restore path above)
EM_VISITED guard: Wave-B needs this for forward refs in indented SAVE blocks
//

NEW EM_VISITED
NEW EF_VSCLEAR
NEW EMIT_FUNC_SECONDARY
NEW RT_BUILD_ENTRY
NEW RTB_LUX_BODY
