//
commands.re -- every command handler in one place (merged from expand.re +
synth.re; parser.re's NEW/SET/LINK/LOAD/NOVA handlers appended below).
Handlers all build their NOVA graph the same way now (WAX_CMD dispatch ->
run handler -> mint/wire luces via SYNTH_* / WRITE_NOVA_SLOTS). Shared
builder infra (WRITE_NOVA_SLOTS, autolink, LINK_NEXT, GL_IMPL) rides along.
Pure parser/dispatch machinery stays in parser.re.
//

════════ PART 1 -- was expand.re (SWITCH / FOR / SAVE / NOLINK / NOVA reg) ════════
//
expand.re — self-hosted substitutes for what Python statically expands
/from text (loader.py's _expand_indent and _wave2_wire_line)
/
/The dividing line from synth.re is NOT "self-hosted vs Python" and NOT
/"reads a body vs takes plain arguments" -- both were wrong guesses made
/(and corrected) while sorting this out. The real question is narrower
/and purely mechanical: does loader.py itself already generate this
/construct's instructions directly from source text, somewhere (either
/_expand_indent's own block-opener dispatch -- SWITCH/FOR/NOVAE/MATCH/
/PACK_BYTE/UNPACK_BYTE/DEF/SAVE/WRITE_OUT/READ_BODY, all of
/_SPECIAL_OPENERS -- or _wave2_wire_line's primitive handling -- NEW/
/SET/NOVA/SEAL/RVOCA/RREDI/NOLINK/JEQ/JZ)?
/
/If yes: Python already does this job for every file it loads (cold
/boot's own files included), so nothing here is a second copy of a
/shared mechanism -- it exists *only* because PS_MAIN (parser.re) is
/self-hosted Reca code that cannot call into Python, and needs its own
/equivalent for exactly the same constructs Python already handles
/everywhere else. EXP_SWITCH_START, EXP_CMD_FOR_IMPL (+ its LOAD_CF_*
/family), EXP_CMD_NOVA, LOAD_CMD_SEAL, EXP_CMD_RVOCA_IMPL,
/EXP_CMD_RREDI_IMPL, EXP_CMD_NOLINK live here for this reason.
/
/If no -- nothing in loader.py generates this construct's instructions
/from text, ever, for anyone, cold-boot files included -- then it's a
/genuinely shared, runtime-callable builder (WAX_CMD + BS_HTAB lookup +
/JumpReg/RVOCA, real interpreter execution, not Python text-rewriting),
/and it belongs in synth.re instead: RCALL_AT, WALK_ONE, LINK_OP,
/UNLINK_OP, EMIT/EMITI/PUTBYTE, LH/LR/LT/LX, YAKU. (SPAN used to belong
/here too -- deleted along with its whole implementation, see below.)
/
/LOAD_CMD_UNKNOWN, LOAD_READARG_KV, and LOAD_DISPATCH_LINE/CORE/
/BUILT_LINE stay in parser.re -- they're the dispatch mechanism itself
/(what the parser *is*), not a specific construct it expands, and
/nothing about them is macro-shaped.

────────────────────────────────────────────────────────────────
//

/── SWITCH: direct-build runtime builder ──────────────────────────────
/EXP_SWITCH_START is dispatched as an ordinary command (WAX_CMD SWITCH),
/in the one build pass, exactly like SAVE/FOR. Each case builds its JEQ
/luces DIRECTLY, by calling the same
/SYNTH_JEQ runtime builder a hand-written "JEQ ..." line uses -- no
/"JEQ ..." text is emitted to a buffer and re-parsed. This removes the
/serialize->reparse round-trip the old text path had: SWITCH and a
/hand-written JEQ now reach the identical builder, the only difference
/being that SWITCH sets RA_MA0..RA_MA4 from resolved case tokens instead
/of from the macro-call preamble.
/
/reg/val/dest are resolved through the SAME LOAD_ELEM_RESOLVE every
/keyed/positional argument goes through (int -> C_N addr, symbol ->
/LOAD_INTERN with forward-ref backfill, string -> packed chain). A
/case value like "SP" or "0" becomes the El2 address of an Equal exactly
/as a hand-written JEQ's third argument would; a forward-referenced dest
/backfills into RA_SW_DEST_ADDR, the same register-target shape a
/positional macro argument uses (see LOAD_READARG_KV's LOAD_RKVA_PSI*).
/
/Line formats (identical to before, and to loader.py's _expand_indent):
/  "VAL DEST"            -> one value -> dest (no arrow)
/  "V1 V2 > DEST"        -> several values, same dest
/  "V1 V2 V3 V4 > DEST"  -> up to four values, same dest
/All SWITCH usages project-wide are anonymous (just "SWITCH reg"), so the
/JEQ luces need no generated textual name at all: SWITCH_BUILD_JEQ mints
/two anonymous NOVA luces for each JEQ (name + _J) and hands their
/addresses to SYNTH_JEQ as At/Next. The old __sw_N / NAME_i text-name
/schemes (SWITCH_RESOLVE_NAME, WRITE_DECIMAL usage) are gone with the
/text path -- nothing referenced those names.

NEW RA_SW_REG_ADDR   /resolved addr of the switched-on register
NEW RA_SW_DEST_ADDR  /resolved addr of the current case's destination
NEW SK_SW_VALS  Count=8      /resolved value addrs for the current case line
NEW SK_SW_VALS_BASE In=OWN   /base addr of SK_SW_VALS (set once, via Idem)
NEW SK_SW_NV In=OWN          /count of values collected on the current line
NEW SK_SW_BI In=OWN          /build-loop index over SK_SW_VALS
NEW SK_SW_BJ_VAL In=OWN      /SWITCH_BUILD_JEQ input: value addr for this JEQ
NEW SK_SW_BJ_NAME In=OWN     /SWITCH_BUILD_JEQ scratch: minted name-node addr
NEW SK_SW_BJ_JN In=OWN       /SWITCH_BUILD_JEQ scratch: minted _J-node addr
SET SW_ARROW_BYTE Value=62 /'>'

NOLINK
/── Openers without a self-hosted handler yet (MATCH, DEF) ──────────────
/These declare that MATCH and DEF open an indented body (Open=<mode>), the
/single source both the Python loader (_discover_block_openers) and the
/self-hosted parser read to know an opener. The Python side already builds
/both (MATCH via _match_expand, DEF via _def_expand_records); the Reca-side
/EXP_* handler is a placeholder (LOAD_CMD_UNKNOWN) until parser.re is wired --
/added deliberately now so the opener set has one home, per the Open= design.
RVOCA EXP_MATCH_START Body=LOAD_CMD_UNKNOWN Cmd=MATCH Open=match
NOLINK
RVOCA EXP_DEF_START Body=LOAD_CMD_UNKNOWN Cmd=DEF Open=def

NOLINK
/── Header: "SWITCH reg [NAME]" -- read reg, consume the rest of the
/header line (any NAME token plus the trailing LF), then resolve reg.
/BS_SKIP_TO_EOL runs BEFORE the resolve so it consumes from the byte
/stream while RA_LOAD_BYTE still holds reg's own terminator; BS_TOKBUF
/still holds the reg token for the resolve immediately after (skip does
/not touch the token buffer). A NAME, if present, is intentionally
/ignored -- direct-built luces carry no textual name.
RVOCA EXP_SWITCH_START Body=LOAD_READ_TOKEN Cmd=SWITCH Open=switch
RVOCA EXP_SW_HDR_SKIP Body=BS_SKIP_TO_EOL
NOVA EXP_SW_REG_EX Op=Idem El1=RA_SW_REG_ADDR Exit=SK_ELEM_EXIT
RVOCA EXP_SW_REG_RES Body=LOAD_ELEM_RESOLVE
NOVA EXP_SW_REG_SET Op=Move El1=SK_ELEM_VALUE Exit=RA_SW_REG_ADDR
/Publish the value-array base once (Idem = the array's own address, no
/deref), so per-line indexing can Add straight off it.
NOVA EXP_SW_VB Op=Idem El1=SK_SW_VALS Exit=SK_SW_VALS_BASE
/Push SWITCH context: saves enclosing indent state, reinits indent
/stack rooted at this header's indent.
NOVA EXP_SW_CMODE Op=Move El1=CTX_MODE_SWITCH Exit=SK_TMP3
RVOCA EXP_SW_CPUSH Body=LOAD_CTX_PUSH
RVOCA EXP_SW_RDLINE Body=LOAD_READ_LINE
/EOF ends the block.
JZ EXP_SW_EOFCK El1=RA_LOAD_BYTE Exit=EXP_SW_DONE
/LOAD_READ_LINE left the exact indent in SK_IND_COUNT; LOAD_INDENT_MARK
/classifies it and derives SK_IND_DEPTH (0 = not indented -> block ends).
RVOCA EXP_SW_MARKC Body=LOAD_INDENT_MARK
JZ EXP_SW_INDCK El1=SK_IND_DEPTH Exit=EXP_SW_DONE
/── Parse one case line: collect value addrs until '>' or end of line,
/then the dest. No arrow -> the last collected token WAS the dest. ──
CLEAR EXP_SW_NV0 El1=SK_SW_NV
RVOCA EXP_SW_VLOOP Body=LOAD_READ_TOKEN
/Empty token -> end of this line.
JZ EXP_SW_VEMCK El1=RA_LOAD_TLEN Exit=EXP_SW_LINEDONE
/Arrow? (a lone '>' -- no case value ever begins with it.)
NOVA EXP_SW_VB0 Op=Read El1=BS_TOKBUF_BASE Exit=SK_TMP2
JEQ EXP_SW_ARRCK El1=SK_TMP2 El2=SW_ARROW_BYTE Exit=EXP_SW_SEENARROW
/Ordinary value: resolve, append to SK_SW_VALS[SK_SW_NV].
NOVA EXP_SW_VSLOT Op=Add El1=SK_SW_VALS_BASE El2=SK_SW_NV Exit=SK_TMP
NOVA EXP_SW_VEX Op=Move El1=SK_TMP Exit=SK_ELEM_EXIT
RVOCA EXP_SW_VRES Body=LOAD_ELEM_RESOLVE
NOVA EXP_SW_VSLOT2 Op=Add El1=SK_SW_VALS_BASE El2=SK_SW_NV Exit=SK_TMP
NOVA EXP_SW_VWRITE Op=Write El1=SK_TMP El2=SK_ELEM_VALUE
NOVA EXP_SW_VINC Op=Add El1=SK_SW_NV El2=C_1 Exit=SK_SW_NV
NOVA EXP_SW_VJMP Op=Jump Exit=EXP_SW_VLOOP
NOLINK
/Arrow seen: the next token is the destination. Resolve it (forward refs
/backfill into RA_SW_DEST_ADDR), consume the rest of the line, build.
NOVA EXP_SW_SEENARROW Op=Move El1=BS_TOKBUF_BASE Exit=SK_TMP
RVOCA EXP_SW_DTOK Body=LOAD_READ_TOKEN
NOVA EXP_SW_DEX Op=Idem El1=RA_SW_DEST_ADDR Exit=SK_ELEM_EXIT
RVOCA EXP_SW_DRES Body=LOAD_ELEM_RESOLVE
NOVA EXP_SW_DSET Op=Move El1=SK_ELEM_VALUE Exit=RA_SW_DEST_ADDR
RVOCA EXP_SW_DSKIP Body=BS_SKIP_TO_EOL
NOVA EXP_SW_DJMP Op=Jump Exit=EXP_SW_BUILD
NOLINK
/No arrow: line ended after the value tokens. The last one is really the
/dest -- pop it off the value array (canonical "VAL DEST" form; the
/1-value case leaves SK_SW_NV at 0 and builds nothing, matching the old
/path's no-real-case edge).
JZ EXP_SW_LINEDONE El1=SK_SW_NV Exit=EXP_SW_RDLINE
NOVA EXP_SW_POPDEC Op=Sub El1=SK_SW_NV El2=C_1 Exit=SK_SW_NV
NOVA EXP_SW_POPADDR Op=Add El1=SK_SW_VALS_BASE El2=SK_SW_NV Exit=SK_TMP
NOVA EXP_SW_POPRD Op=Read El1=SK_TMP Exit=RA_SW_DEST_ADDR
/── Build: one JEQ per collected value, all to RA_SW_DEST_ADDR. ──
CLEAR EXP_SW_BUILD El1=SK_SW_BI
JEQ EXP_SW_BLOOP El1=SK_SW_BI El2=SK_SW_NV Exit=EXP_SW_RDLINE
NOVA EXP_SW_BADDR Op=Add El1=SK_SW_VALS_BASE El2=SK_SW_BI Exit=SK_TMP
NOVA EXP_SW_BRD Op=Read El1=SK_TMP Exit=SK_SW_BJ_VAL
RVOCA EXP_SW_BDO Body=SWITCH_BUILD_JEQ
NOVA EXP_SW_BINC Op=Add El1=SK_SW_BI El2=C_1 Exit=SK_SW_BI
NOVA EXP_SW_BJMP Op=Jump Exit=EXP_SW_BLOOP
NOLINK
/Pop SWITCH context: restores enclosing indent state.
/Both exit JEQs (EOFCK, INDCK) jump to EXP_SW_DONE, so the pop
/must BE that label, not precede it.
RVOCA EXP_SW_DONE Body=LOAD_CTX_POP
RREDI EXP_SW_RET

── SWITCH_BUILD_JEQ: build one JEQ lux pair directly via SYNTH_JEQ ──
/IN:  SK_SW_BJ_VAL   = El2 (compare-value) address for this case
/     RA_SW_REG_ADDR = El1 (switched register) address
/     RA_SW_DEST_ADDR = JumpIf destination address (0 if a fwd ref not
/                       yet backfilled -- accepted, non-fundamental, same
/                       as any positional macro arg's forward ref)
/Mints two anonymous NOVA luces (name + _J), the same two a hand-written
/JEQ's alloc would have produced, and hands them to SYNTH_JEQ as
/At=RA_MA0 / Next=RA_MA4. SYNTH_JEQ does the autolink from RA_MC_PREV, so
/consecutive cases chain into the fall-through dispatch exactly like a
/run of hand-written JEQ lines.
SET SWITCH_BUILD_JEQ Value=SW_BJ_IMPL
NOLINK
/Mint name lux (NOVA_SIZE, no extra lumina).
NOVA SW_BJ_ABASE Op=Move El1=NOVA_SIZE Exit=RA_ALLOC_BASE
CLEAR SW_BJ_ATF El1=RA_ALLOC_TERM_FORCE
CLEAR SW_BJ_LC El1=SK_TMP
RVOCA SW_BJ_A1 Body=LOAD_ALLOC_SIZED
NOVA SW_BJ_NAME Op=Move El1=RA_ALLOC_RESULT Exit=SK_SW_BJ_NAME
/Mint _J lux.
NOVA SW_BJ_ABASE2 Op=Move El1=NOVA_SIZE Exit=RA_ALLOC_BASE
CLEAR SW_BJ_ATF2 El1=RA_ALLOC_TERM_FORCE
CLEAR SW_BJ_LC2 El1=SK_TMP
RVOCA SW_BJ_A2 Body=LOAD_ALLOC_SIZED
NOVA SW_BJ_JN Op=Move El1=RA_ALLOC_RESULT Exit=SK_SW_BJ_JN
/Set the builder args and call the shared JEQ builder.
NOVA SW_BJ_M0 Op=Move El1=SK_SW_BJ_NAME Exit=RA_MA0
NOVA SW_BJ_M1 Op=Move El1=RA_SW_REG_ADDR Exit=RA_MA1
NOVA SW_BJ_M2 Op=Move El1=SK_SW_BJ_VAL Exit=RA_MA2
NOVA SW_BJ_M3 Op=Move El1=RA_SW_DEST_ADDR Exit=RA_MA3
NOVA SW_BJ_M4 Op=Move El1=SK_SW_BJ_JN Exit=RA_MA4
RVOCA SW_BJ_BUILD Body=SYNTH_JEQ
RREDI SW_BJ_RET
── EXP_CMD_FOR_IMPL: FOR slot1 slot2 ... — full v2 (FOR_V2_DESIGN.md) ─
//
Self-hosted implementation of the FOR macro. Header parsing (named +
numeric count-block slots into RA_FOR_IRIS_BUF), override-table
resolution (three priority passes: default, position/range, name --
see the FOR register block comment above (this file) for why), then per-slot
expansion via the pre-existing LOAD_EXPAND_TEMPLATE/LOAD_READ_BODY
(BUGS.md Pattern Z: these existed as unfinished scaffolding --
referenced but never-declared registers, and one dispatch bug of
their own, both fixed alongside this).
'F' also covers FREE (0 real top-level usages project-wide as of this
session's own check) -- distinguished by second character, the same
way any same-first-letter command names are told apart elsewhere in
this file (SET vs SAVE, both starting 'S').
//
NOLINK
NOVA EXP_CMD_FOR_IMPL Op=Add El1=BS_TOKBUF_BASE El2=C_1 Exit=SK_TMP Cmd=FOR Open=for
NOVA EXP_CF_B1 Op=Read El1=SK_TMP Exit=SK_TMP
JEQ EXP_CF_OCK El1=SK_TMP El2=ASCII_O Exit=EXP_CF_START
RVOCA EXP_CF_UCMD Body=LOAD_CMD_UNKNOWN
RREDI EXP_CF_URET
NOLINK
/Reset all FOR-local state for this block.
CLEAR EXP_CF_START El1=RA_FOR_TOTAL
NOVA EXP_CF_IPI Op=Move El1=RA_FOR_IRIS_BUF Exit=RA_FOR_IRIS_PTR
NOVA EXP_CF_OPI Op=Move El1=RA_FOR_OV_BUF Exit=RA_FOR_OV_PTR
NOVA EXP_CF_ONI Op=Move El1=RA_FOR_OV_NODES Exit=RA_FOR_OV_NODE_PTR
NOVA EXP_CF_IBA Op=Move El1=RA_FOR_IRIS_BUF Exit=RA_FOR_IRIS_BASE_ADDR
NOVA EXP_CF_IIBA Op=Move El1=RA_FOR_IRIS_IDX_BASE Exit=RA_FOR_IRIS_IDX_BASE_ADDR
NOVA EXP_CF_OHBA Op=Move El1=RA_FOR_OV_HEAD_BASE Exit=RA_FOR_OV_HEAD_BASE_ADDR
CLEAR EXP_CF_HCI El1=SK_CF_HIDX
JEQ EXP_CF_HCK El1=SK_CF_HIDX El2=C_256 Exit=EXP_CF_HDONE
NOVA EXP_CF_HCA Op=Add El1=RA_FOR_OV_HEAD_BASE_ADDR El2=SK_CF_HIDX Exit=SK_TMP2
NOVA EXP_CF_HCW Op=Write El1=SK_TMP2 El2=C_0
NOVA EXP_CF_HCINC Op=Add El1=SK_CF_HIDX El2=C_1 Exit=SK_CF_HIDX
NOVA EXP_CF_HCJ Op=Jump Exit=EXP_CF_HCK
NOLINK
/── Header token loop: each token is either a numeric count (N
/anonymous slots) or one named slot. ──
NOVA EXP_CF_HDONE Op=Jump Exit=EXP_CF_HLOOP
NOLINK
RVOCA EXP_CF_HLOOP Body=BS_READ_TOKEN
JZ EXP_CF_HTCK El1=RA_LOAD_TLEN Exit=EXP_CF_HEND
/Is this token purely numeric? Scan its own bytes.
CLEAR EXP_CF_NCI El1=SK_CF_NIDX
CLEAR EXP_CF_NCI2 El1=SK_CF_ISNUM
NOVA EXP_CF_NSET Op=Move El1=C_1 Exit=SK_CF_ISNUM
JEQ EXP_CF_NCK El1=SK_CF_NIDX El2=RA_LOAD_TLEN Exit=EXP_CF_NDONE
NOVA EXP_CF_NADDR Op=Add El1=BS_TOKBUF_BASE El2=SK_CF_NIDX Exit=SK_TMP2
NOVA EXP_CF_NBYTE Op=Read El1=SK_TMP2 Exit=SK_TMP
NOVA EXP_CF_NSUB Op=Sub El1=SK_TMP El2=ASCII_0 Exit=SK_TMP
NOVA EXP_CF_NRANGE Op=ULess El1=SK_TMP El2=C_10 Exit=SK_FLAG
NOVA EXP_CF_NRJMP Op=JumpIf El1=SK_FLAG Exit=EXP_CF_NADV
NOVA EXP_CF_NCLR Op=Move El1=C_0 Exit=SK_CF_ISNUM
NOVA EXP_CF_NADV Op=Add El1=SK_CF_NIDX El2=C_1 Exit=SK_CF_NIDX
NOVA EXP_CF_NJMP Op=Jump Exit=EXP_CF_NCK
NOLINK
JZ EXP_CF_NDONE El1=SK_CF_ISNUM Exit=EXP_CF_HNAME
/Numeric: parse as integer, add that many anonymous slots.
RVOCA EXP_CF_NPARSE Body=BS_PARSE_INT
CLEAR EXP_CF_ACI El1=SK_CF_AIDX
JEQ EXP_CF_ACK El1=SK_CF_AIDX El2=RA_BS_PIVAL Exit=EXP_CF_AAJMP
RVOCA EXP_CF_AADD Body=EXP_CF_ADD_ANON
NOVA EXP_CF_AAINC Op=Add El1=SK_CF_AIDX El2=C_1 Exit=SK_CF_AIDX
NOVA EXP_CF_AAJ Op=Jump Exit=EXP_CF_ACK
NOLINK
NOVA EXP_CF_AAJMP Op=Jump Exit=EXP_CF_HLOOP
NOLINK
/Named: copy the token's own bytes into the IRIS pool, one slot.
RVOCA EXP_CF_HNAME Body=EXP_CF_ADD_NAMED
NOVA EXP_CF_HDONEJ Op=Jump Exit=EXP_CF_HLOOP
NOLINK
NOVA EXP_CF_HEND Op=Jump Exit=EXP_CF_CPUSH
NOLINK
/Push FOR context onto the context stack. This saves the enclosing
/indent state and reinits the indent stack rooted at this FOR header's
/own indent (SK_IND_COUNT still holds it, from LOAD_COUNT_INDENT).
/EXP_CF_HEND above jumps HERE, not to EXP_CF_RB.
NOVA EXP_CF_CMODE Op=Move El1=CTX_MODE_FOR Exit=SK_TMP3
RVOCA EXP_CF_CPUSH Body=LOAD_CTX_PUSH
RVOCA EXP_CF_RB Body=LOAD_READ_BODY
RVOCA EXP_CF_PO Body=EXP_CF_PARSE_OVERRIDES
CLEAR EXP_CF_SLI El1=RA_FOR_IDX
JEQ EXP_CF_SLCK El1=RA_FOR_IDX El2=RA_FOR_TOTAL Exit=EXP_CF_SLDONE
NOVA EXP_CF_SLEA Op=Add El1=RA_FOR_IRIS_IDX_BASE_ADDR El2=RA_FOR_IDX Exit=SK_TMP2
NOVA EXP_CF_SLER Op=Read El1=SK_TMP2 Exit=RA_FOR_ELEM
RVOCA EXP_CF_SLEXP Body=LOAD_EXPAND_TEMPLATE
NOVA EXP_CF_SLINC Op=Add El1=RA_FOR_IDX El2=C_1 Exit=RA_FOR_IDX
NOVA EXP_CF_SLJ Op=Jump Exit=EXP_CF_SLCK
NOLINK
/Pop FOR context: restores enclosing indent state from the context stack.
/JEQ EXP_CF_SLCK jumps here when the loop finishes, so the pop must
/carry the SLDONE label.
RVOCA EXP_CF_SLDONE Body=LOAD_CTX_POP
RREDI EXP_CF_RET

── EXP_CF_ADD_ANON: append one anonymous slot to the IRIS pool ──
//
Anonymous slot = zero-length name (immediate NULL terminator).
IN: RA_FOR_TOTAL (slot position to record), RA_FOR_IRIS_PTR (bump ptr).
Advances both.
//
NOLINK
NOVA EXP_CF_ADD_ANON Op=Add El1=RA_FOR_IRIS_IDX_BASE_ADDR El2=RA_FOR_TOTAL Exit=SK_TMP2
NOVA EXP_CFAA_WIDX Op=Write El1=SK_TMP2 El2=RA_FOR_IRIS_PTR
NOVA EXP_CFAA_WNUL Op=Write El1=RA_FOR_IRIS_PTR El2=C_0
NOVA EXP_CFAA_ADV Op=Add El1=RA_FOR_IRIS_PTR El2=C_1 Exit=RA_FOR_IRIS_PTR
NOVA EXP_CFAA_TINC Op=Add El1=RA_FOR_TOTAL El2=C_1 Exit=RA_FOR_TOTAL
RREDI EXP_CFAA_RET

── EXP_CF_ADD_NAMED: append one named slot (current token) to the IRIS pool ──
//
IN: BS_TOKBUF_BASE/RA_LOAD_TLEN (the token just read), RA_FOR_TOTAL,
RA_FOR_IRIS_PTR. Advances both.
//
NOLINK
NOVA EXP_CF_ADD_NAMED Op=Add El1=RA_FOR_IRIS_IDX_BASE_ADDR El2=RA_FOR_TOTAL Exit=SK_TMP2
NOVA EXP_CFAN_WIDX Op=Write El1=SK_TMP2 El2=RA_FOR_IRIS_PTR
CLEAR EXP_CFAN_CI El1=SK_CF_CIDX
JEQ EXP_CFAN_CK El1=SK_CF_CIDX El2=RA_LOAD_TLEN Exit=EXP_CFAN_CDONE
NOVA EXP_CFAN_SRC Op=Add El1=BS_TOKBUF_BASE El2=SK_CF_CIDX Exit=SK_TMP2
NOVA EXP_CFAN_BYTE Op=Read El1=SK_TMP2 Exit=SK_FLAG
NOVA EXP_CFAN_DST Op=Add El1=RA_FOR_IRIS_PTR El2=SK_CF_CIDX Exit=SK_TMP2
NOVA EXP_CFAN_WR Op=Write El1=SK_TMP2 El2=SK_FLAG
NOVA EXP_CFAN_CINC Op=Add El1=SK_CF_CIDX El2=C_1 Exit=SK_CF_CIDX
NOVA EXP_CFAN_CJMP Op=Jump Exit=EXP_CFAN_CK
NOLINK
NOVA EXP_CFAN_CDONE Op=Add El1=RA_FOR_IRIS_PTR El2=RA_LOAD_TLEN Exit=RA_FOR_IRIS_PTR
NOVA EXP_CFAN_WNUL Op=Write El1=RA_FOR_IRIS_PTR El2=C_0
NOVA EXP_CFAN_ADV Op=Add El1=RA_FOR_IRIS_PTR El2=C_1 Exit=RA_FOR_IRIS_PTR
NOVA EXP_CFAN_TINC Op=Add El1=RA_FOR_TOTAL El2=C_1 Exit=RA_FOR_TOTAL
RREDI EXP_CFAN_RET

── EXP_CF_OV_FIND_NAME_SLOT: resolve a name selector to its own slot ──
//
IN: SK_CF_OV_LSTART/SK_CF_OV_SEL_LEN (the name text, within the body
buffer). OUT: SK_CF_OV_SLOTLO=SK_CF_OV_SLOTHI = matching slot position,
or SK_CF_OV_SLOTLO=C_MAXU (not found -- an unresolved selector, a
build-time error per FOR_V2_DESIGN.md; the caller reports and halts).
Compares byte-for-byte against every slot's own IRIS-stored name --
O(slots * name_len) per selector, fine at this project's FOR-block
scale (a handful of slots, a handful of overrides each).
//
NOLINK
CLEAR EXP_CF_OV_FIND_NAME_SLOT El1=SK_CF_OV_SLOT
JEQ EXP_CFFN_SCK El1=SK_CF_OV_SLOT El2=RA_FOR_TOTAL Exit=EXP_CFFN_NOTFOUND
NOVA EXP_CFFN_IDXA Op=Add El1=RA_FOR_IRIS_IDX_BASE_ADDR El2=SK_CF_OV_SLOT Exit=SK_TMP2
NOVA EXP_CFFN_IDXR Op=Read El1=SK_TMP2 Exit=SK_TMP
NOVA EXP_CFFN_NADDR Op=Add El1=RA_FOR_IRIS_BASE_ADDR El2=SK_TMP Exit=SK_TMP2
/Compare this slot's own name (NULL-terminated at SK_TMP2) against the
/selector text (SK_CF_OV_LSTART, length SK_CF_OV_SEL_LEN).
CLEAR EXP_CFFN_CCI El1=SK_CF_CIDX
JEQ EXP_CFFN_CCK El1=SK_CF_CIDX El2=SK_CF_OV_SEL_LEN Exit=EXP_CFFN_CCLENCK
NOVA EXP_CFFN_CCA Op=Add El1=SK_TMP2 El2=SK_CF_CIDX Exit=SK_TMP3
NOVA EXP_CFFN_CCR Op=Read El1=SK_TMP3 Exit=SK_FLAG
JZ EXP_CFFN_CCZ El1=SK_FLAG Exit=EXP_CFFN_NEXT
NOVA EXP_CFFN_SB Op=Add El1=SK_CF_OV_LSTART El2=SK_CF_CIDX Exit=SK_TMP3
NOVA EXP_CFFN_SBR Op=Read El1=SK_TMP3 Exit=SK_TMP3
JEQ EXP_CFFN_EQCK El1=SK_FLAG El2=SK_TMP3 Exit=EXP_CFFN_NEXT
NOVA EXP_CFFN_CCINC Op=Add El1=SK_CF_CIDX El2=C_1 Exit=SK_CF_CIDX
NOVA EXP_CFFN_CCJ Op=Jump Exit=EXP_CFFN_CCK
NOLINK
/Reached selector's own length with every byte matching -- confirm the
/candidate slot's own name is EXACTLY this long too (its own next byte
/must be NULL), not just a matching prefix.
NOVA EXP_CFFN_CCLENCK Op=Add El1=SK_TMP2 El2=SK_CF_OV_SEL_LEN Exit=SK_TMP3
NOVA EXP_CFFN_CCLR Op=Read El1=SK_TMP3 Exit=SK_FLAG
JZ EXP_CFFN_LENOK El1=SK_FLAG Exit=EXP_CFFN_NEXT
NOVA EXP_CFFN_MATCHOK Op=Move El1=SK_CF_OV_SLOT Exit=SK_CF_OV_SLOTHI
NOVA EXP_CFFN_LOK2 Op=Move El1=C_1 Exit=SK_CF_OV_FOUND
RREDI EXP_CFFN_RET
NOLINK
NOVA EXP_CFFN_NEXT Op=Add El1=SK_CF_OV_SLOT El2=C_1 Exit=SK_CF_OV_SLOT
NOVA EXP_CFFN_NEXTJ Op=Jump Exit=EXP_CFFN_SCK
NOLINK
NOVA EXP_CFFN_NOTFOUND Op=Move El1=C_0 Exit=SK_CF_OV_FOUND
RREDI EXP_CFFN_NFRET

── EXP_CF_PARSE_UINT: parse an unsigned int from an arbitrary byte range ──
//
IN: SK_CF_UI_ADDR (start), SK_CF_UI_LEN (byte count). OUT: RA_BS_PIVAL.
Unlike BS_PARSE_INT (which reads from BS_TOKBUF_BASE specifically),
this works on any address range -- needed for parsing selector numbers
embedded within the FOR override table's own stored body text.
//
NOLINK
CLEAR EXP_CF_PARSE_UINT El1=RA_BS_PIVAL
CLEAR EXP_CFPU_CI El1=SK_CF_CIDX
JEQ EXP_CFPU_CK El1=SK_CF_CIDX El2=SK_CF_UI_LEN Exit=EXP_CFPU_DONE
NOVA EXP_CFPU_M10 Op=Mul El1=RA_BS_PIVAL El2=C_10 Exit=RA_BS_PIVAL
NOVA EXP_CFPU_ADDR Op=Add El1=SK_CF_UI_ADDR El2=SK_CF_CIDX Exit=SK_TMP3
NOVA EXP_CFPU_BYTE Op=Read El1=SK_TMP3 Exit=SK_TMP3
NOVA EXP_CFPU_SUB Op=Sub El1=SK_TMP3 El2=ASCII_0 Exit=SK_TMP3
NOVA EXP_CFPU_ADD Op=Add El1=RA_BS_PIVAL El2=SK_TMP3 Exit=RA_BS_PIVAL
NOVA EXP_CFPU_CINC Op=Add El1=SK_CF_CIDX El2=C_1 Exit=SK_CF_CIDX
NOVA EXP_CFPU_CJMP Op=Jump Exit=EXP_CFPU_CK
NOLINK
RREDI EXP_CFPU_DONE

── EXP_CF_OV_CLASSIFY_SEL: classify a selector, resolve its slot range ──
//
IN: SK_CF_OV_LSTART (line start), SK_CF_OV_SEL_LEN (selector length).
OUT: SK_CF_OV_PRI (0/1/2), SK_CF_OV_SLOTLO/SK_CF_OV_SLOTHI.
Empty -> default (0, all slots). Contains only digits -> position (1).
Digits-MINUS-digits -> range (1). Anything else -> name (2), resolved
via EXP_CF_OV_FIND_NAME_SLOT.
//
NOLINK
JZ EXP_CF_OV_CLASSIFY_SEL El1=SK_CF_OV_SEL_LEN Exit=EXP_CFCS_DEFAULT
/Scan for a '-' (range) and confirm every other byte is a digit.
CLEAR EXP_CFCS_SI El1=SK_CF_CIDX
CLEAR EXP_CFCS_SI2 El1=SK_CF_OV_KVPOS
NOVA EXP_CFCS_DPOS Op=Move El1=C_0 Exit=SK_CF_OV_KVPOS
JEQ EXP_CFCS_SCK El1=SK_CF_CIDX El2=SK_CF_OV_SEL_LEN Exit=EXP_CF_OV_PRI_SET1
NOVA EXP_CFCS_SADDR Op=Add El1=SK_CF_OV_LSTART El2=SK_CF_CIDX Exit=SK_TMP3
NOVA EXP_CFCS_SBYTE Op=Read El1=SK_TMP3 Exit=SK_TMP3
JEQ EXP_CFCS_MINUSCK El1=SK_TMP3 El2=MINUS Exit=EXP_CFCS_MARKMINUS
NOVA EXP_CFCS_SUB2 Op=Sub El1=SK_TMP3 El2=ASCII_0 Exit=SK_TMP3
NOVA EXP_CFCS_RNG Op=ULess El1=SK_TMP3 El2=C_10 Exit=SK_FLAG
JZ EXP_CFCS_NDCK El1=SK_FLAG Exit=EXP_CFCS_ISNAME
NOVA EXP_CFCS_SADV Op=Jump Exit=EXP_CFCS_SNEXT
NOLINK
NOVA EXP_CFCS_MARKMINUS Op=Move El1=SK_CF_CIDX Exit=SK_CF_OV_KVPOS
NOVA EXP_CFCS_SNEXT Op=Add El1=SK_CF_CIDX El2=C_1 Exit=SK_CF_CIDX
NOVA EXP_CFCS_SJMP Op=Jump Exit=EXP_CFCS_SCK
NOLINK
/Every byte was a digit or exactly one dash -- and a dash, if present,
/was recorded at SK_CF_OV_KVPOS. Position (no dash seen -> KVPOS still
/its init value 0, ambiguous with "dash at position 0" -- but a
/selector can't legitimately start with '-', so 0 unambiguously means
/"no dash found" here) or range.
NOVA EXP_CF_OV_PRI_SET1 Op=Move El1=C_1 Exit=SK_CF_OV_PRI
JZ EXP_CFCS_MINUSCK2 El1=SK_CF_OV_KVPOS Exit=EXP_CFCS_POSONLY
/Range: dash at SK_CF_OV_KVPOS. LOW = [0,KVPOS), HIGH = (KVPOS,SEL_LEN).
NOVA EXP_CFCS_LOADR Op=Idem El1=SK_CF_OV_LSTART Exit=SK_CF_UI_ADDR
NOVA EXP_CFCS_LOLEN Op=Move El1=SK_CF_OV_KVPOS Exit=SK_CF_UI_LEN
RVOCA EXP_CFCS_LOPRS Body=EXP_CF_PARSE_UINT
NOVA EXP_CFCS_LOSV Op=Move El1=RA_BS_PIVAL Exit=SK_CF_OV_SLOTLO
NOVA EXP_CFCS_HIA Op=Add El1=SK_CF_OV_LSTART El2=SK_CF_OV_KVPOS Exit=SK_TMP3
NOVA EXP_CFCS_HIA2 Op=Add El1=SK_TMP3 El2=C_1 Exit=SK_CF_UI_ADDR
NOVA EXP_CFCS_HILEN Op=Sub El1=SK_CF_OV_SEL_LEN El2=SK_CF_OV_KVPOS Exit=SK_TMP3
NOVA EXP_CFCS_HILEN2 Op=Sub El1=SK_TMP3 El2=C_1 Exit=SK_CF_UI_LEN
RVOCA EXP_CFCS_HIPRS Body=EXP_CF_PARSE_UINT
NOVA EXP_CFCS_HISV Op=Move El1=RA_BS_PIVAL Exit=SK_CF_OV_SLOTHI
RREDI EXP_CFCS_RRET
NOLINK
NOVA EXP_CFCS_POSONLY Op=Idem El1=SK_CF_OV_LSTART Exit=SK_CF_UI_ADDR
NOVA EXP_CFCS_POLEN Op=Move El1=SK_CF_OV_SEL_LEN Exit=SK_CF_UI_LEN
RVOCA EXP_CFCS_POPRS Body=EXP_CF_PARSE_UINT
NOVA EXP_CFCS_POSV Op=Move El1=RA_BS_PIVAL Exit=SK_CF_OV_SLOTLO
NOVA EXP_CFCS_POSV2 Op=Move El1=RA_BS_PIVAL Exit=SK_CF_OV_SLOTHI
RREDI EXP_CFCS_PORET
NOLINK
NOVA EXP_CFCS_ISNAME Op=Move El1=C_2 Exit=SK_CF_OV_PRI
RVOCA EXP_CFCS_NMFIND Body=EXP_CF_OV_FIND_NAME_SLOT
RREDI EXP_CFCS_NMRET
NOLINK
NOVA EXP_CFCS_SDONE Op=Move El1=C_2 Exit=SK_CF_OV_PRI
RVOCA EXP_CFCS_SDFIND Body=EXP_CF_OV_FIND_NAME_SLOT
RREDI EXP_CFCS_SDRET
NOLINK
NOVA EXP_CFCS_DEFAULT Op=Move El1=C_0 Exit=SK_CF_OV_PRI
NOVA EXP_CFCS_DEFLO Op=Move El1=C_0 Exit=SK_CF_OV_SLOTLO
NOVA EXP_CFCS_DEFHI Op=Sub El1=RA_FOR_TOTAL El2=C_1 Exit=SK_CF_OV_SLOTHI
RREDI EXP_CFCS_DRET

── EXP_CF_OV_APPLY_ELEM: parse key=value pairs, apply to slot range ──
//
IN: SK_CF_OV_KVPOS (start addr of the key=value text, reused here as
a plain address -- distinct meaning from its "dash position" use in
EXP_CF_OV_CLASSIFY_SEL, which has already returned by this point),
SK_CF_OV_LLEN-relative end, SK_CF_OV_SLOTLO/SLOTHI (inclusive slot
range). Space-separated "key=value" pairs. For each pair, for each
slot in range, prepends a new node to that slot's own override list
(RA_FOR_OV_HEAD_BASE[slot]).
//
NOLINK
JEQ EXP_CF_OV_APPLY_ELEM El1=SK_CF_OV_KVPOS El2=SK_CF_OV_KVEND Exit=EXP_CFAK_DONE
NOVA EXP_CFAK_HIP1 Op=Add El1=SK_CF_OV_SLOTHI El2=C_1 Exit=SK_CF_OV_SLOTHIP1
/Skip leading spaces
NOVA EXP_CFAK_SKB Op=Read El1=SK_CF_OV_KVPOS Exit=SK_TMP3
JEQ EXP_CFAK_SPCK El1=SK_TMP3 El2=SP Exit=EXP_CFAK_SKADV
/Find '=' (end of key) and the pair's own end (next space or KVEND)
NOVA EXP_CFAK_KSTART Op=Move El1=SK_CF_OV_KVPOS Exit=SK_CF_OV_KSTART
CLEAR EXP_CFAK_EQI El1=SK_CF_OV_EQPOS
NOVA EXP_CFAK_EQI2 Op=Move El1=SK_CF_OV_KVPOS Exit=SK_CF_OV_EQPOS
JEQ EXP_CFAK_EQCK El1=SK_CF_OV_KVPOS El2=SK_CF_OV_KVEND Exit=EXP_CFAK_PAIREND
NOVA EXP_CFAK_EQR Op=Read El1=SK_CF_OV_KVPOS Exit=SK_TMP3
JEQ EXP_CFAK_EQBCK El1=SK_TMP3 El2=EQUALS Exit=EXP_CFAK_FOUNDEQ
JEQ EXP_CFAK_SPCK2 El1=SK_TMP3 El2=SP Exit=EXP_CFAK_PAIREND
NOVA EXP_CFAK_KVADV Op=Add El1=SK_CF_OV_KVPOS El2=C_1 Exit=SK_CF_OV_KVPOS
NOVA EXP_CFAK_KVJ Op=Jump Exit=EXP_CFAK_EQCK
NOLINK
NOVA EXP_CFAK_FOUNDEQ Op=Move El1=SK_CF_OV_KVPOS Exit=SK_CF_OV_EQPOS
NOVA EXP_CFAK_FEQADV Op=Add El1=SK_CF_OV_KVPOS El2=C_1 Exit=SK_CF_OV_KVPOS
NOVA EXP_CFAK_FEQJ Op=Jump Exit=EXP_CFAK_EQCK
NOLINK
/Reached KVEND or a space with no '=' found yet -- if EQPOS never
/advanced past KSTART, there was no '=' at all; skip this malformed
/segment rather than crash.
NOVA EXP_CFAK_PAIREND Op=Move El1=SK_CF_OV_KVPOS Exit=SK_CF_OV_VEND
JEQ EXP_CFAK_MALCK El1=SK_CF_OV_EQPOS El2=SK_CF_OV_KSTART Exit=EXP_CFAK_SKADV2
/key = [KSTART, EQPOS), value = [EQPOS+1, VEND)
NOVA EXP_CFAK_KLEN Op=Sub El1=SK_CF_OV_EQPOS El2=SK_CF_OV_KSTART Exit=SK_CF_OV_KLEN
NOVA EXP_CFAK_VSTART Op=Add El1=SK_CF_OV_EQPOS El2=C_1 Exit=SK_CF_OV_VSTART
NOVA EXP_CFAK_VLEN Op=Sub El1=SK_CF_OV_VEND El2=SK_CF_OV_VSTART Exit=SK_CF_OV_VLEN
/For each slot in [SLOTLO, SLOTHI], prepend one node for this pair.
NOVA EXP_CFAK_SLI Op=Move El1=SK_CF_OV_SLOTLO Exit=SK_CF_OV_SLOT
JEQ EXP_CFAK_SLCK El1=SK_CF_OV_SLOT El2=SK_CF_OV_SLOTHIP1 Exit=EXP_CFAK_SLDONE
RVOCA EXP_CFAK_MKNODE Body=EXP_CF_OV_MAKE_NODE
NOVA EXP_CFAK_SLINC Op=Add El1=SK_CF_OV_SLOT El2=C_1 Exit=SK_CF_OV_SLOT
NOVA EXP_CFAK_SLJ Op=Jump Exit=EXP_CFAK_SLCK
NOLINK
NOVA EXP_CFAK_SLDONE Op=Jump Exit=EXP_CFAK_SKADV2
NOLINK
NOVA EXP_CFAK_SKADV Op=Add El1=SK_CF_OV_KVPOS El2=C_1 Exit=SK_CF_OV_KVPOS
NOVA EXP_CFAK_SKADVJ Op=Jump Exit=EXP_CF_OV_APPLY_ELEM
NOLINK
NOVA EXP_CFAK_SKADV2 Op=Add El1=SK_CF_OV_KVPOS El2=C_1 Exit=SK_CF_OV_KVPOS
NOVA EXP_CFAK_SKADVJ2 Op=Jump Exit=EXP_CF_OV_APPLY_ELEM
NOLINK
RREDI EXP_CFAK_DONE

── EXP_CF_OV_MAKE_NODE: allocate+link one override node ──
//
IN: SK_CF_OV_KSTART/SK_CF_OV_KLEN (key text), SK_CF_OV_VSTART/
SK_CF_OV_VLEN (value text), SK_CF_OV_SLOT (target slot). Prepends to
RA_FOR_OV_HEAD_BASE[slot]. Node layout: key_addr,key_len,val_addr,
val_len,next -- 5 luces, from RA_FOR_OV_NODES.
//
NOLINK
NOVA EXP_CF_OV_MAKE_NODE Op=Move El1=RA_FOR_OV_NODE_PTR Exit=SK_TMP3
NOVA EXP_CFMN_W0 Op=Write El1=SK_TMP3 El2=SK_CF_OV_KSTART
NOVA EXP_CFMN_A1 Op=Add El1=SK_TMP3 El2=C_1 Exit=SK_TMP2
NOVA EXP_CFMN_W1 Op=Write El1=SK_TMP2 El2=SK_CF_OV_KLEN
NOVA EXP_CFMN_A2 Op=Add El1=SK_TMP3 El2=C_2 Exit=SK_TMP2
NOVA EXP_CFMN_W2 Op=Write El1=SK_TMP2 El2=SK_CF_OV_VSTART
NOVA EXP_CFMN_A3 Op=Add El1=SK_TMP3 El2=C_3 Exit=SK_TMP2
NOVA EXP_CFMN_W3 Op=Write El1=SK_TMP2 El2=SK_CF_OV_VLEN
NOVA EXP_CFMN_HDA Op=Add El1=RA_FOR_OV_HEAD_BASE_ADDR El2=SK_CF_OV_SLOT Exit=SK_CF_OV_HDADDR
NOVA EXP_CFMN_HDR Op=Read El1=SK_CF_OV_HDADDR Exit=SK_FLAG
NOVA EXP_CFMN_A4 Op=Add El1=SK_TMP3 El2=C_4 Exit=SK_TMP2
NOVA EXP_CFMN_W4 Op=Write El1=SK_TMP2 El2=SK_FLAG
NOVA EXP_CFMN_HDW Op=Write El1=SK_CF_OV_HDADDR El2=SK_TMP3
NOVA EXP_CFMN_ADVN Op=Add El1=RA_FOR_OV_NODE_PTR El2=C_5 Exit=RA_FOR_OV_NODE_PTR
RREDI EXP_CFMN_RET

── EXP_CF_PARSE_OVERRIDES: resolve all override lines, 3 priority passes ──
//
Pass 0=default, 1=position/range, 2=name -- see FOR_V2_DESIGN.md's own
"most specific wins, last-in-text breaks ties" rule and the register
block comment above (this file) for why 3 ordered passes implement this
without per-entry priority comparison. Each pass walks the whole body
buffer in text order, processing only C_2 (override) lines whose own
classified priority matches the current pass.
//
NOLINK
CLEAR EXP_CF_PARSE_OVERRIDES El1=SK_CF_OV_PASS
JEQ EXP_CFPO_PCK El1=SK_CF_OV_PASS El2=C_3 Exit=EXP_CFPO_DONE
NOVA EXP_CFPO_PI Op=Move El1=SK_BODY_BASE Exit=SK_CF_OV_PTR
NOVA EXP_CFPO_LCK Op=ULess El1=SK_CF_OV_PTR El2=SK_BODY_PTR Exit=SK_FLAG
NOVA EXP_CFPO_LCKJI Op=JumpIf El1=SK_FLAG Exit=EXP_CFPO_MK
NOVA EXP_CFPO_LCKJ Op=Jump Exit=EXP_CFPO_LEND
NOVA EXP_CFPO_MK Op=Read El1=SK_CF_OV_PTR Exit=SK_TMP
NOVA EXP_CFPO_MKI Op=Add El1=SK_CF_OV_PTR El2=C_1 Exit=SK_CF_OV_PTR
NOVA EXP_CFPO_LSSET Op=Move El1=SK_CF_OV_PTR Exit=SK_CF_OV_LSTART
/Find this line's own end (LF), regardless of its marker.
JEQ EXP_CFPO_ECK El1=SK_CF_OV_PTR El2=SK_BODY_PTR Exit=EXP_CFPO_EDONE
NOVA EXP_CFPO_EB Op=Read El1=SK_CF_OV_PTR Exit=SK_TMP3
JEQ EXP_CFPO_ELFCK El1=SK_TMP3 El2=LF Exit=EXP_CFPO_EDONE
NOVA EXP_CFPO_EADV Op=Add El1=SK_CF_OV_PTR El2=C_1 Exit=SK_CF_OV_PTR
NOVA EXP_CFPO_EJ Op=Jump Exit=EXP_CFPO_ECK
NOLINK
NOVA EXP_CFPO_EDONE Op=Sub El1=SK_CF_OV_PTR El2=SK_CF_OV_LSTART Exit=SK_CF_OV_LLEN
JEQ EXP_CFPO_C2CK El1=SK_TMP El2=C_2 Exit=EXP_CFPO_SKIPLINE
/Override line. Find '>' within [LSTART, LSTART+LLEN).
CLEAR EXP_CFPO_GI El1=SK_CF_CIDX
CLEAR EXP_CFPO_GI2 El1=SK_CF_OV_GTPOS
NOVA EXP_CFPO_GINIT Op=Move El1=C_0 Exit=SK_CF_OV_GTPOS
JEQ EXP_CFPO_GCK El1=SK_CF_CIDX El2=SK_CF_OV_LLEN Exit=EXP_CFPO_SKIPLINE
NOVA EXP_CFPO_GADDR Op=Add El1=SK_CF_OV_LSTART El2=SK_CF_CIDX Exit=SK_TMP3
NOVA EXP_CFPO_GBYTE Op=Read El1=SK_TMP3 Exit=SK_TMP3
JEQ EXP_CFPO_GTCK El1=SK_TMP3 El2=GREATER Exit=EXP_CFPO_GFOUND
NOVA EXP_CFPO_GADV Op=Add El1=SK_CF_CIDX El2=C_1 Exit=SK_CF_CIDX
NOVA EXP_CFPO_GJ Op=Jump Exit=EXP_CFPO_GCK
NOLINK
NOVA EXP_CFPO_GFOUND Op=Move El1=SK_CF_CIDX Exit=SK_CF_OV_SEL_LEN
RVOCA EXP_CFPO_CLS Body=EXP_CF_OV_CLASSIFY_SEL
JEQ EXP_CFPO_PMCK El1=SK_CF_OV_PRI El2=SK_CF_OV_PASS Exit=EXP_CFPO_SKIPLINE
/This line's own priority matches the current pass -- apply its
/key=value pairs. KV text is [SEL_LEN+1, LLEN) relative to LSTART.
NOVA EXP_CFPO_KVS Op=Add El1=SK_CF_OV_LSTART El2=SK_CF_OV_SEL_LEN Exit=SK_TMP3
NOVA EXP_CFPO_KVS2 Op=Add El1=SK_TMP3 El2=C_1 Exit=SK_CF_OV_KVPOS
NOVA EXP_CFPO_KVE Op=Add El1=SK_CF_OV_LSTART El2=SK_CF_OV_LLEN Exit=SK_CF_OV_KVEND
RVOCA EXP_CFPO_AKV Body=EXP_CF_OV_APPLY_ELEM
NOVA EXP_CFPO_SKIPLINE Op=Add El1=SK_CF_OV_PTR El2=C_1 Exit=SK_CF_OV_PTR
NOVA EXP_CFPO_SKJ Op=Jump Exit=EXP_CFPO_LCK
NOLINK
NOVA EXP_CFPO_LEND Op=Add El1=SK_CF_OV_PASS El2=C_1 Exit=SK_CF_OV_PASS
NOVA EXP_CFPO_LEJ Op=Jump Exit=EXP_CFPO_PCK
NOLINK
RREDI EXP_CFPO_DONE

/EXP_CMD_NOVA now lives below in this file (PART 3), moved from parser.re
/(next to PS_CMD_INSTR/PS_LK_SLOT, the two implementations they
/replaced there) -- parser.re owns grammar parsing per its own file
/header; this file owns wave orchestration. WAX_CMD's own registration
/below still points at EXP_CMD_NOVA by name; Reca addresses don't care
/which file a label's text sits in.

/LOAD_CMD_SEAL and its LOAD_CSEAL_* body (SEAL's own self-hosted
/implementation) are removed entirely -- SEAL doesn't exist as a
/command anymore (retired as a pure subset of SET, see loader.py),
/and nothing else called into this body. (LOAD_POOL_NAME, once mentioned
/here, is also gone -- forward refs record an HT entry address now, not a
/pooled name copy. See LOAD_INTERN in parser.re.)




── EXP_CMD_NOLINK: suppress next autolink ───────────────────
/Clears RA_MC_PREV so the very next NOVA skips autolink. Captures the
/current value into BS_LAST_NOVA first -- the one place that register
/exists for: letting a macro that explicitly wants "the real last
/instruction, surviving a NOLINK" recover it (Move BS_LAST_NOVA ->
/RA_MC_PREV, opt-in, at its own call site) even though RA_MC_PREV
/itself no longer carries it forward. Nothing reads BS_LAST_NOVA today
/-- this file no longer reseeds RA_MC_PREV from it before every macro
/dispatch (that was a real bug: LOAD_CU_BSYNC below used to write back
/the pre-call value, not the post-call one, making the whole reseed/
/sync round-trip a no-op every time -- and even fixed, it would have
/discarded RA_MC_PREV's own correct in-flight progress on every single
/macro call for no benefit, since RA_MC_PREV already flows forward
/correctly on its own, the same way Python's _last_nova does).
NOLINK
NOVA EXP_CMD_NOLINK Op=Move El1=RA_MC_PREV Exit=BS_LAST_NOVA Cmd=NOLINK
/Close the chain properly before dropping it: the tail's flow stops here,
/and "stops here" is an address (LUX_END), not an absent value. Slot 5
/being 0 already means "my successor is my physical neighbour", so a tail
/left at 0 does not stop -- it inherits whatever lux allocation happened
/to put next to it. That is what silently rerouted the comment stripper's
/drop path. Same rule the Python loader applies at its own chain breaks.
JZ EXP_NL_PREVCK El1=RA_MC_PREV Exit=EXP_NL_CLRPV
NOVA EXP_NL_ENDP Op=Move El1=RA_MC_PREV Exit=RA_NL_PREV
NOVA EXP_NL_ENDN Op=Move El1=LUX_END Exit=RA_NL_NEXT
RVOCA EXP_NL_ENDLNK Body=LINK_NEXT
CLEAR EXP_NL_CLRPV El1=RA_MC_PREV
RVOCA EXP_NL_SKP Body=BS_SKIP_TO_EOL
RREDI EXP_NL_RET


//
────────────────────────────────────────────────────────────────
Below: EXP_SAVE_START, moved here from macros.re. SAVE is in
_SPECIAL_OPENERS (loader.py) -- Python already expands it directly
from text for every file it loads; this is PS_MAIN's own self-hosted
equivalent, needed for the same reason as EXP_SWITCH_START/EXP_CMD_FOR_IMPL
above. RA_SV_REGS/RA_SV_COUNT moved with it (SAVE's own scratch, not
shared with anything else).
────────────────────────────────────────────────────────────────
//

NEW RA_SV_REGS  /SAVE: packed register list
NEW RA_SV_COUNT  /SAVE: number of registers to save
/── SAVE: PUSH-model block handler ────────────────────────────
/MA1..MA4 = register addrs to save/restore.
/RA_LINK must NEVER be passed here -- it is preserved automatically by the
/call stack (Voca/Redi push/pop on RA_SP) at any nesting depth.
/Emits save NOVAs (Move reg->S_reg), pushes CTX_MODE_SAVE context,
/persists RA_MA1..4 and SK_SES_SREG_1..4 in the data_ptr area.
/Body lines dispatched by LOAD_DISPATCH_LINE; restore NOVAs
/emitted on dedent by LOAD_CTX_CLOSE_TOP.
NOLINK
RVOCA EXP_SAVE_START Body=SAVE_EMIT_SAVES Cmd=SAVE Open=save
/push CTX_MODE_SAVE context (saves indent state, reinits indent stack)
NOVA EXP_SAVE_MODE Op=Move El1=CTX_MODE_SAVE Exit=SK_TMP3
RVOCA EXP_SAVE_PUSH Body=LOAD_CTX_PUSH
/allocate data area for persisted SAVE state (SAVE_DATA_SIZE words)
ALLOC_TO EXP_SAVE_ALLOC Exit=SK_TMP Count=SAVE_DATA_SIZE
/write data_ptr into slot[CTX_OFF_DATA_PTR]
NOVA EXP_SAVE_DPA Op=Add El1=SK_CTX_SLOT_ADDR El2=CTX_OFF_DATA_PTR Exit=SK_TMP2
NOVA EXP_SAVE_DPW Op=Write El1=SK_TMP2 El2=SK_TMP
/persist RA_MA1..4 into data[0..3]
NOVA EXP_SAVE_SM1 Op=Write El1=SK_TMP El2=RA_MA1
NOVA EXP_SAVE_O1 Op=Add El1=SK_TMP El2=C_1 Exit=SK_TMP2
NOVA EXP_SAVE_SM2 Op=Write El1=SK_TMP2 El2=RA_MA2
NOVA EXP_SAVE_O2 Op=Add El1=SK_TMP El2=C_2 Exit=SK_TMP2
NOVA EXP_SAVE_SM3 Op=Write El1=SK_TMP2 El2=RA_MA3
NOVA EXP_SAVE_O3 Op=Add El1=SK_TMP El2=C_3 Exit=SK_TMP2
NOVA EXP_SAVE_SM4 Op=Write El1=SK_TMP2 El2=RA_MA4
/persist SK_SES_SREG_1..4 values into data[4..7]
NOVA EXP_SAVE_O4 Op=Add El1=SK_TMP El2=C_4 Exit=SK_TMP2
NOVA EXP_SAVE_RS1 Op=Read El1=SK_SES_SREG_1 Exit=SK_TMP3
NOVA EXP_SAVE_SS1 Op=Write El1=SK_TMP2 El2=SK_TMP3
NOVA EXP_SAVE_O5 Op=Add El1=SK_TMP El2=C_5 Exit=SK_TMP2
NOVA EXP_SAVE_RS2 Op=Read El1=SK_SES_SREG_2 Exit=SK_TMP3
NOVA EXP_SAVE_SS2 Op=Write El1=SK_TMP2 El2=SK_TMP3
NOVA EXP_SAVE_O6 Op=Add El1=SK_TMP El2=C_6 Exit=SK_TMP2
NOVA EXP_SAVE_RS3 Op=Read El1=SK_SES_SREG_3 Exit=SK_TMP3
NOVA EXP_SAVE_SS3 Op=Write El1=SK_TMP2 El2=SK_TMP3
NOVA EXP_SAVE_O7 Op=Add El1=SK_TMP El2=C_7 Exit=SK_TMP2
NOVA EXP_SAVE_RS4 Op=Read El1=SK_SES_SREG_4 Exit=SK_TMP3
NOVA EXP_SAVE_SS4 Op=Write El1=SK_TMP2 El2=SK_TMP3
RREDI EXP_SAVE_RET
/── LOAD_*/SAVE_EMIT_* subroutines are implemented in wax.re. No forward-
/declaration stub needed here: Wave A allocates every NEW/SEAL/
/NOVA name from every file before Wave B wires anything (see
/auto_discover's docstring: "Two-phase makes ordering irrelevant... Forward
/references resolved automatically"), so wax.re's real implementations
/are already addressable by the time macros.re's RVOCA calls reference
/them by name, regardless of file processing order. A stub-then-overwrite
/indirection used to sit here; it was redundant with this guarantee and,
/worse, never actually got correctly overwritten in the final build (see
/BUGS.md) -- every one of its 8 names ended up permanently stuck pointing
/at its own no-op stub instead of the real wax.re implementation.
/============================================================

//
────────────────────────────────────────────────────────────────
Below: NOVA/NOLINK/FOR's own WAX_CMD registrations and FOR's own scratch
registers -- moved here from macros.re, which is now empty and deleted.
Mechanical move, not a rewrite. (The LOAD_COPY_LINE/WRITE_DECIMAL/
WRITE_PACKED_NAME text utilities that also lived here served only
SWITCH's old text-name generation; they were removed with the direct-
build conversion -- nothing else called them.)
────────────────────────────────────────────────────────────────
//

/── NOVA: dispatched to expand.re's EXP_CMD_NOVA ──────────────────
/An NOVA_START implementation used to live here: it took already-parsed
/arguments in MA0..MA4, allocated NOVA_SIZE luces, wired the slots and
/autolinked. expand.re's EXP_CMD_NOVA does all of that and more -- it
/reads the line itself, accepts Name= in any position, sizes the
/allocation to include room for the name's future LINK lumina, and
/reuses an existing lux when the name is already known. The two were
/never both reachable: LOAD_DISPATCH_CORE's first-byte branch sent
/every NOVA line to EXP_CMD_NOVA, so NOVA_START was live but
/unreachable, and pointing the table at it would have silently
/downgraded every NOVA to the weaker path. One implementation now, and
/the table names it.

/── NOLINK: break autolink attachment, but remember the last real NOVA ──
/MA0 unused. Captures RA_MC_PREV into BS_LAST_NOVA before clearing --
/BS_LAST_NOVA exists solely so a macro that explicitly wants "the real
/last instruction, surviving a NOLINK" can recover it (Move BS_LAST_NOVA
/-> RA_MC_PREV, opt-in, at its own call site). Nothing reads it today;
/RA_MC_PREV flowing forward through nested calls is the desired
/behavior everywhere except right after NOLINK -- for an unrelated
/unattached scratch use of RA_MC_PREV inside some future macro body,
/use SAVE instead, not this (see expand.re's EXP_CMD_NOLINK for the
/top-level-dispatch counterpart of this same mechanism).
/A NOLINK_SV/NOLINK_START implementation used to live here: capture
/RA_MC_PREV into BS_LAST_NOVA, then clear it. expand.re's
/EXP_CMD_NOLINK does exactly that and also consumes the rest of the
/line, which a top-level command must do. The two were never both
/reachable -- LOAD_DISPATCH_CORE's first-byte branch sent every NOLINK
/line to EXP_CMD_NOLINK -- so this one was live but unreachable, and
/pointing the table at it would have left the line half-read. One
/implementation now, and the table names it.
/── Scratch registers for FOR/SAVE (SWITCH's own moved to synth.re) ──
NEW RA_FOR_IDX  /FOR: current iteration index
NEW RA_FOR_ELEM  /FOR: current element addr
NEW RA_FOR_BODY  /FOR: addr of body template start
/Registers for the full FOR v2 implementation (FOR_V2_DESIGN.md):
/named+anonymous slots, count blocks, override tables. See BUGS.md
/Pattern Z for why LOAD_EXPAND_TEMPLATE referenced RA_FOR_IRIS_BUF/
/RA_FOR_IRIS_IDX_BASE before either was ever declared -- partial,
/never-finished scaffolding from an earlier session.
NEW RA_FOR_TOTAL  /total slot count (named + anonymous) this FOR
NEW RA_FOR_IRIS_BUF  Count=2048  /packed slot-name text pool, NULL-terminated
                            /per slot; anonymous slots get an immediate
                            /NULL (zero-length name)
NEW RA_FOR_IRIS_PTR  /bump pointer into RA_FOR_IRIS_BUF
NEW RA_FOR_IRIS_IDX_BASE  Count=256  /per-slot offset into RA_FOR_IRIS_BUF
                                 /(index = slot position, 0..255 slots max)
/Override resolution: processed in three passes by priority --
/default lines first, then position/range, then name (each pass in
/text order) -- so a later pass's own entry, prepended to the front of
/its slot's own list, is always found first at lookup time. This
/gives "most specific wins, last-in-text breaks ties within the same
/specificity" (FOR_V2_DESIGN.md) without needing per-entry priority
/comparison at write time: priority is entirely encoded by *when* an
/entry gets prepended, not stored alongside it.
/List node layout (4 luces): key_addr, key_len, val_addr, val_len, next.
NEW RA_FOR_OV_BUF  Count=4096  /override key/value text pool
NEW RA_FOR_OV_PTR  /bump pointer into RA_FOR_OV_BUF
NEW RA_FOR_OV_NODES  Count=1280  /node storage: 256 slots * 5 luces/node headroom
NEW RA_FOR_OV_NODE_PTR  /bump pointer into RA_FOR_OV_NODES
NEW RA_FOR_OV_HEAD_BASE  Count=256  /per-slot head-of-list pointer (0 = no
                                /overrides for this slot yet)
/FOR name buffer: stores element names as packed strings for {X} substitution
SET FOR_IRIS_BUF_SIZE Value=2048
NEW FOR_IRIS_BUF_000  Count=2048  /name bytes storage
NEW RA_FOR_IRIS_BUF   /base of name buffer
NEW RA_FOR_IRIS_PTR   /write position in name buffer
NEW RA_FOR_IRIS_LEN   /length of current element name in bytes
/Name index table: FOR_IRIS_IDX_000[i] = offset into name buf for element i
SET FOR_IRIS_IDX_SIZE Value=8
NEW FOR_IRIS_IDX_000  Count=8  /offsets for up to 7 elements
NEW RA_FOR_IRIS_IDX_BASE    /base of name index table

/── FOR: dispatched to expand.re's EXP_CMD_FOR_IMPL ─────────────
/A FOR_START/FOR_ITER_ELEM implementation used to live here. It read
/its elements from MA1..MA7 -- capped at seven, positional only, no
/override block -- and looped calling FOR_ITER_ELEM, itself just
/LOAD_EXPAND_TEMPLATE plus an index bump. expand.re's
/EXP_CMD_FOR_IMPL parses the header itself with any number of slots,
/named and anonymous, and supports the override table; it calls the
/same LOAD_EXPAND_TEMPLATE directly. The two were never both
/reachable -- LOAD_DISPATCH_CORE's first-byte branch sent every FOR
/line to EXP_CMD_FOR_IMPL -- so this one was live but unreachable,
/and pointing the table at it would have silently capped FOR at seven
/positional elements. One implementation now, and the table names it.

NOLINK



════════ PART 2 -- was synth.re (NOVAE synthesis + JEQ/JZ/RVOCA/RREDI/CLEAR/NOP/ALLOC_TO/LX/LH/LR/LT + shared infra) ════════
/synth.re — runtime instruction synthesis (NOVAE and everything built on it)
/
/Everything in this file is a genuinely callable Reca subroutine (RVOCA/
/RREDI, standard call stack) that SYNTHESIZES new NOVA luces while a
/program runs -- not a text-level macro expanded once while a file is
/being read. That split is the whole reason this file exists separately
/from macros.re (which now holds only load-time-text macros: SWITCH's
/own text-driven form, FOR/SPAN's dispatch stubs to wax.re, SAVE, YAKU,
/CHAIN-adjacent helpers) and from expand.re (pure Python-side text
/macros with no Reca-callable body at all: JEQ/JZ as ordinary source
/text, SWITCH/FOR/MATCH/PACK_BYTE/UNPACK_BYTE as loader.py's own
/_expand_indent constructs).
/
/Every builder below is registered the same way (WAX_CMD Name=X X_SPN),
/takes its arguments via RA_MA0.. (the project-wide macro-call
/convention), and is meant to be called from anywhere -- cold-boot
/dispatch (wax.re), a hot-loaded module (parser.re/PS_MAIN), or another
/builder. Before this file existed, PS_MAIN had grown its own complete,
/independent copy of most of what's here (PS_MK_RCALL_PATTERN and
/siblings) on the belief that "runtime" code couldn't reuse "load-time"
/macros -- a belief this project has since settled is false (see
/BUGS.md/HISTORY.md): there is one prepass, not two separate worlds,
/and every builder here was already written to be called from either.
/
/Load-time aspects (freeze-only), same as macros.re used to document:
/  __LT_GET_LAST — RA_MA_RET = _last_nova
/  __LT_SET_LAST — _last_nova = aether[RA_MA0]
/  __LT_REG_SYM  — register symbol name(MA0)=lux(MA1) in Python symbols
/
/NOVA slot offsets: C_1=op C_2=e1 C_3=e2 C_4=exit C_5=next
/Write El1=REG El2=VAL → aether[aether[REG]] = aether[VAL]

NEW RA_MC_LUX  /scratch: current lux being built
NEW RA_MC_PREV  /scratch: previous lux for auto-link (synced with loader._last_nova)
NEW RA_MC_SLOT  /scratch: slot address (lux_addr + offset)
NEW RA_MC_EXTRA_COUNT  /scratch: how many extra (rel,val) lumen pairs
                    /__LT_ALLOC_NOVA should reserve beyond NOVA_SIZE, on top of
                    /the fixed 7 slots -- 0 (the default) means an ordinary,
                    /unextended NOVA. Set by a NOVAE line's own emitted Move
                    /right before its __LT_ALLOC_NOVA node, when that line
                    /carries its own arbitrary extra keys (In1=/Op1=/... on a
                    /NOVAE without At=, see loader.py's NOVAE Wave 2 handling).
                    /__LT_ALLOC_NOVA reads and clears it on every call, so it
                    /never leaks into a later call site that set nothing.
NEW __LT_ALLOC_NOVA  /load-time only: bump-allocate one anonymous NOVA lux, return addr in RA_MA_RET
                    /Python injects the real handler into interpreter dispatch during _call_reca_macro.
                    /At runtime this lux is dead (handler removed after each macro call).

/── WIRE_AUTOLINK: wire RA_MC_LUX (op/e1/e2/exit) then autolink prev→lux ────
/Caller sets: RA_MC_LUX, RA_MC_OP, RA_MC_E1, RA_MC_E2, RA_MC_DEST, then RVOCA.
/Writes all 5 slots, then calls AUTOLINK (updates RA_MC_PREV).
SET WIRE_AUTOLINK Value=SYNTH_WRITE_NOVA_SLOTS /alias: same body as WRITE_NOVA_SLOTS
/── RVOCA: RVOCA name sub → NOVA name Voca El1=sub Exit=RA_LINK
/MA0=name_addr  MA1=sub_addr
NOVAE SYNTH_RVOCA Cmd=RVOCA Op=Voca El1=RA_MA1 Exit=RA_LINK At=RA_MA0 Op3=Idem

RREDI SYNTH_RVOCA_RET
/── RREDI: RREDI name → NOVA name Redi El1=RA_LINK + reset _last_nova
/MA0=name_addr
NOVAE SYNTH_RREDI Cmd=RREDI Op=Redi El1=RA_LINK At=RA_MA0 Reset=1 Op1=Idem

RREDI SYNTH_RREDI_RET
/── CLEAR: CLEAR name target → NOVA name Move El1=C_0 Exit=target
/MA0=name_addr  MA1=target_addr
NOVAE SYNTH_CLEAR Cmd=CLEAR Op=Move El1=C_0 Exit=RA_MA1 At=RA_MA0 Op1=Idem

RREDI SYNTH_CLEAR_RET
/── NOP: NOP name → NOVA name Move El1=C_0 Exit=C_0
/MA0=name_addr
NOVAE SYNTH_NOP Cmd=NOP Op=Move El1=C_0 Exit=C_0 At=RA_MA0 Op1=Idem Op3=Idem

RREDI SYNTH_NOP_RET
/── ALLOC_TO: ALLOC_TO name dest count → Move count→RA_ALLOC_COUNT; ALLOC; Move result→dest
/MA0=name MA1=dest MA2=count
/── ALLOC_TO name dest size ───────────────────────────────────────
/Synthesizes 3 NOVAs: name(Move size→ALLOC_COUNT), _J(Voca ALLOC), _K(Move RESULT→dest).
/N1 is the entry (At=RA_MA0 = the caller's name). _J/_K are anonymous -> no At=,
/so they allocate fresh + autolink (same shape as RCALL_AT/UNPACK_BYTE). The old
/form threaded them through RA_MA3/RA_MA4 as if auto-completed, but nothing
/allocated those (only JEQ/JZ have an auto-complete) -- the LINK/GL latent bug.
NOLINK
NOVAE SYNTH_ALLOC_TO Cmd=ALLOC_TO Op=Move El1=RA_MA2 Exit=RA_ALLOC_COUNT At=RA_MA0 Op3=Idem

NOVAE SYNTH_ALLOC_TO_N2 Op=Voca El1=ALLOC Exit=RA_LINK Op3=Idem

NOVAE SYNTH_ALLOC_TO_N3 Op=Move El1=RA_ALLOC_RESULT Exit=RA_MA1 Op1=Idem

RREDI SYNTH_ALLOC_TO_RET

/── WRITE_OUT n val → Write val to [LD_NI_OUT], then bump LD_NI_OUT ──────────
/Mirrors loader.py _CmdWRITE_OUT. MA0=n (name), MA1=val.
/N1 (the named entry, At=RA_MA0): Write El1=LD_NI_OUT El2=val -- LD_NI_OUT is a
/fixed register (its own address is the operand, so Op1=Idem), val=RA_MA1 is a
/resolved arg (its value is the operand, default Move). N2 (n_i, anonymous ->
/no At=, fresh + autolink): Add LD_NI_OUT,C_1 -> LD_NI_OUT, all fixed (Idem).
NOLINK
NOVAE SYNTH_WRITE_OUT Cmd=WRITE_OUT Op=Write El1=LD_NI_OUT El2=RA_MA1 At=RA_MA0 Op1=Idem

NOVAE SYNTH_WRITE_OUT_N2 Op=Add El1=LD_NI_OUT El2=C_1 Exit=LD_NI_OUT Op1=Idem Op2=Idem Op3=Idem

RREDI SYNTH_WRITE_OUT_RET

/── ALLOC_RAW [name] size target → Move size→ALLOC_COUNT; Voca ALLOC_RAW_SUB;
/── Move result→target ──────────────────────────────────────────────────────
/Mirrors loader.py _CmdALLOC_RAW. The leading name is optional: `ALLOC_RAW
/size target` (2 args, anonymous entry) or `ALLOC_RAW name size target` (3
/args, named entry -- a real jump target, e.g. P0_ALLOC_EPBUF). LOAD_READARG_KV
/has already put the raw tokens in MA0.. positionally, so the imperative prefix
/normalizes them to MA0=name MA1=size MA2=target, minting a fresh anonymous
/entry into MA0 for the 2-arg form (nothing references it, so a load-pass
/fresh lux is fine -- same as ALLOC_TO's own anonymous sub-nodes). Then the
/uniform 3-node NOVAE triple emits, identical in shape to ALLOC_TO (N1 named
/At=RA_MA0, N2/N3 anonymous no-At), only calling ALLOC_RAW_SUB (zeroing) and
/reading size from MA1 / writing the base to MA2.
NOLINK
NOVA SYNTH_ALLOC_RAW Cmd=ALLOC_RAW Op=Move El1=RA_MA2 Exit=SK_TMP
JZ SAR_2ARGCK El1=SK_TMP Exit=SAR_SHIFT
NOVA SAR_3ARG_J Op=Jump Exit=SYNTH_ALLOC_RAW_E1
NOLINK
/2-arg form: shift size/target down one and mint an anonymous entry.
NOVA SAR_SHIFT Op=Move El1=RA_MA1 Exit=RA_MA2
NOVA SAR_SH2 Op=Move El1=RA_MA0 Exit=RA_MA1
NOVA SAR_MINT Op=Move El1=NOVA_SIZE Exit=RA_ALLOC_COUNT
RVOCA SAR_ALC Body=ALLOC
NOVA SAR_MINTST Op=Move El1=RA_ALLOC_RESULT Exit=RA_MA0
NOVA SAR_MINTJ Op=Jump Exit=SYNTH_ALLOC_RAW_E1
NOLINK
NOVAE SYNTH_ALLOC_RAW_E1 Op=Move El1=RA_MA1 Exit=RA_ALLOC_COUNT At=RA_MA0 Op3=Idem

NOVAE SYNTH_ALLOC_RAW_E2 Op=Voca El1=ALLOC_RAW_SUB Exit=RA_LINK Op3=Idem

NOVAE SYNTH_ALLOC_RAW_E3 Op=Move El1=RA_ALLOC_RESULT Exit=RA_MA2 Op1=Idem

RREDI SYNTH_ALLOC_RAW_RET

/── UNPACK_BYTE tag word shift byte → Right,And,Add (3 nodes, no JEQ) ────
/Mirrors loader.py _CmdPACK's unpack path: tag(Right word,shift→byte),
/tag_1(And byte,C_255→byte), tag_2(Add shift,C_8→shift). No corpus use
/carries Full=/Exhausted=, so there is no JEQ tail (unlike PACK_BYTE below).
/MA0=tag MA1=word MA2=shift MA3=byte. Sub-nodes are no-At (fresh alloc +
/autolink), same shape as RCALL_AT's own P2.. tail -- no _1/_2 names needed
/since nothing references them.
NOLINK
NOVAE SYNTH_UNPACK_BYTE Cmd=UNPACK_BYTE Op=Right El1=RA_MA1 El2=RA_MA2 Exit=RA_MA3 At=RA_MA0

NOVAE SYNTH_UNPACK_BYTE_N2 Op=And El1=RA_MA3 El2=C_255 Exit=RA_MA3 Op2=Idem

NOVAE SYNTH_UNPACK_BYTE_N3 Op=Add El1=RA_MA2 El2=C_8 Exit=RA_MA2 Op2=Idem

RREDI SYNTH_UNPACK_BYTE_RET

/── PACK_BYTE tag word shift byte Full=L → Left,Or,Add + JEQ(shift,C_64,L) ─
/Mirrors loader.py _CmdPACK (pack path): tag(Left byte,shift→RA_PACK_TMP),
/tag_1(Or word,RA_PACK_TMP→word), tag_2(Add shift,C_8→shift), then a tag_J
/JEQ pair (Equal shift,C_64→flag ; JumpIf flag→Full). Both corpus uses carry
/Full=, so the JEQ is unconditional (add a JZ SK_ARG_Full guard if a no-label
/PACK_BYTE ever appears). MA0=tag MA1=word MA2=shift MA3=byte. Fixed operands
/(RA_PACK_TMP/C_8/C_64) take Idem; the RA_JEQ_FLAG_PTR pointer uses the default
/Move, exactly as SYNTH_JEQ does.
NOLINK
NOVAE SYNTH_PACK_BYTE Cmd=PACK_BYTE Op=Left El1=RA_MA3 El2=RA_MA2 Exit=RA_PACK_TMP At=RA_MA0 Op3=Idem

NOVAE SYNTH_PACK_BYTE_N2 Op=Or El1=RA_MA1 El2=RA_PACK_TMP Exit=RA_MA1 Op2=Idem

NOVAE SYNTH_PACK_BYTE_N3 Op=Add El1=RA_MA2 El2=C_8 Exit=RA_MA2 Op2=Idem

NOVAE SYNTH_PACK_BYTE_JE Op=Equal El1=RA_MA2 El2=C_64 Exit=RA_JEQ_FLAG_PTR Op2=Idem

NOVAE SYNTH_PACK_BYTE_JJ Op=JumpIf El1=RA_JEQ_FLAG_PTR Exit=SK_ARG_Full

RREDI SYNTH_PACK_BYTE_RET

/── HASH_STEP name hashreg bytereg tmpreg toreg → Mul,Add (djb2 step) ──────
/Mirrors loader.py _CmdHASH_STEP: name(Mul hashreg,C_33→tmpreg),
/name_J(Add tmpreg,bytereg→toreg). No declare in Python (only wire), so no
/At=/allowlist here either -- the two nodes are inline, their names are
/referenced nowhere, so they are anonymous fresh nodes (no-At) autolinked
/into the surrounding chain. MA0=name MA1=hashreg MA2=bytereg MA3=tmpreg
/MA4=toreg. Python defaults toreg to hashreg when the 5th arg is absent;
/the three such call sites (lexer.re) were made explicit instead, so no
/runtime default is needed here.
NOLINK
NOVAE SYNTH_HASH_STEP Cmd=HASH_STEP Op=Mul El1=RA_MA1 El2=C_33 Exit=RA_MA3 Op2=Idem

NOVAE SYNTH_HASH_STEP_N2 Op=Add El1=RA_MA3 El2=RA_MA2 Exit=RA_MA4

RREDI SYNTH_HASH_STEP_RET

NEW RA_MC_FLAG  /scratch: flag data lux addr (for JEQ/JZ)
SET RA_MC_FLAG /self-ref
NEW RA_MC_OP  /scratch: op addr
NEW RA_MC_E1  /scratch: arg A
NEW RA_MC_E2  /scratch: arg B
NEW RA_MC_DEST  /scratch: destination addr
NEW RA_MC_J  /scratch: second lux (_J suffix)
NEW RA_MC_K  /scratch: third lux (_K suffix)
NEW RA_MC_SRCREG  /scratch: GL_IMPL -- address of the register src stages into
NEW RA_MC_FUNC  /scratch: GL_IMPL -- address of the SR_GL* function to call
NEW RA_MC_LUMENOP  /scratch: LINK_IMPL -- address of ADD_LUMEN or REMOVE_LUMEN
NEW RA_MC_RESET_FLAG  /scratch: 1 → next AUTOLINK call resets RA_MC_PREV
                            /instead of setting it. Set by WRITE_NOVA_SLOTS_RESET,
                            /consumed and cleared by AUTOLINK itself.
NEW RA_MC_SYNTHFN  /scratch: PS_EMIT_TAIL (parser.re) -- address of the
                        /SYNTH_* builder to call, Idem-computed by the caller
                        /right before jumping to the shared tail
NEW RA_MC_LASTREG  /scratch: PS_EMIT_TAIL (parser.re) -- address of whichever
                        /register the called builder reports its own "last
                        /lux" through (RA_MA4/RA_MC_LUX/RA_MC_PREV/RA_MC_J,
                        /Idem-computed by the caller); read via Read's own
                        /double-dereference, not a fixed register
/── WRITE_NOVA_SLOTS: write all 5 NOVA slots then wire autolink ──
/IN: RA_MC_LUX=addr, RA_MC_OP=op, RA_MC_E1=e1, RA_MC_E2=e2, RA_MC_DEST=exit
/Writes: word(self), op, e1, e2, exit. Then calls WIRE_AUTOLINK.
/Relies solely on the automatic RA_SP call stack to preserve RA_LINK across
/the nested WIRE_AUTOLINK call — safe to call from JEQ/JZ/LINK etc. at any
/nesting depth, with no scratch-register clobbering risk.
NOVA SYNTH_WRITE_NOVA_SLOTS Cmd=WRITE_NOVA_SLOTS Op=Move El1=RA_MC_LUX Exit=RA_MC_SLOT
NOVA Op=Write El1=RA_MC_SLOT El2=RA_MC_LUX
NOVA Op=Add El1=RA_MC_LUX El2=C_1 Exit=RA_MC_SLOT
NOVA Op=Write El1=RA_MC_SLOT El2=RA_MC_OP
NOVA Op=Add El1=RA_MC_LUX El2=C_2 Exit=RA_MC_SLOT
NOVA Op=Write El1=RA_MC_SLOT El2=RA_MC_E1
NOVA Op=Add El1=RA_MC_LUX El2=C_3 Exit=RA_MC_SLOT
NOVA Op=Write El1=RA_MC_SLOT El2=RA_MC_E2
NOVA Op=Add El1=RA_MC_LUX El2=C_4 Exit=RA_MC_SLOT
NOVA Op=Write El1=RA_MC_SLOT El2=RA_MC_DEST
NOVA Op=Voca El1=AUTOLINK Exit=RA_LINK

RREDI SYNTH_WRITE_NOVA_SLOTS_RET

/── WRITE_NOVA_SLOTS_RESET: like WRITE_NOVA_SLOTS but resets RA_MC_PREV after linking ──
/Used by chain terminators (RCALL_AT name_J, RREDI). Sets RA_MC_RESET_FLAG=1
/so the AUTOLINK call inside WRITE_NOVA_SLOTS resets RA_MC_PREV to 0 instead
/of setting it to RA_MC_LUX — same 10-slot-write body, no duplication.
SET WRITE_NOVA_SLOTS_RESET Value=SYNTH_WISR_START
NOLINK
NOVA SYNTH_WISR_START Op=Move El1=C_1 Exit=RA_MC_RESET_FLAG
RVOCA SYNTH_WISR_CALL Body=WRITE_NOVA_SLOTS
RREDI SYNTH_WISR_RET

SET AUTOLINK Value=SYNTH_AUTOLINK_START
/── AUTOLINK: sub — if RA_MC_PREV!=0 AND not physically adjacent to RA_MC_LUX,
/link prev→lux (SLOT_NEXT) via the shared LINK_NEXT primitive (alloc.re) —
/the same adjacency-skip formula LINK_NEXT already implements for RVOCA/
/RREDI/SAVE_EMIT_ONE_SAVE/RESTORE and the NOVA primitive itself. This used
/to reimplement that formula inline (a second copy of the same 5-line
/Add/Equal/JumpIf/Add/Write sequence) — delegated now, one formula.
/Normally sets _last_nova=RA_MC_LUX afterward, regardless of adjacency — UNLESS
/RA_MC_RESET_FLAG is set (by WRITE_NOVA_SLOTS_RESET), in which case _last_nova
/is reset to 0 instead and the flag is cleared. This also merges what used to
/be a second, near-duplicate routine (AUTOLINK_RESET) into one implementation
/— the two only ever differed in this last write.
NOLINK
NOVA SYNTH_AUTOLINK_START Op=Move El1=RA_MC_PREV Exit=RA_NL_PREV
NOVA SYNTH_AUTOLINK_SETNXT Op=Move El1=RA_MC_LUX Exit=RA_NL_NEXT
RVOCA SYNTH_AUTOLINK_LNK Body=LINK_NEXT
NOVA SYNTH_AUTOLINK_SET Op=Move El1=RA_MC_LUX Exit=RA_MC_PREV
/Reset-flag check: normal case (flag==0) falls straight through to RET below,
/no extra cost beyond this one Equal+JumpIf pair.
NOVA SYNTH_AUTOLINK_RSTCK Op=Equal El1=RA_MC_RESET_FLAG El2=C_0 Exit=RA_MC_FLAG
NOVA SYNTH_AUTOLINK_RSTJ Op=JumpIf El1=RA_MC_FLAG Exit=SYNTH_AUTOLINK_RET
NOVA SYNTH_AUTOLINK_RSTW Op=Move El1=C_0 Exit=RA_MC_PREV
NOVA SYNTH_AUTOLINK_RSTCLR Op=Move El1=C_0 Exit=RA_MC_RESET_FLAG
/NOTE: raw NOVA Redi — this is the implementation of the autolink mechanism.
/All users of this mechanism use RREDI. The mechanism itself cannot.
NOVA SYNTH_AUTOLINK_RET Op=Redi El1=RA_LINK
/── JEQ: JEQ name a b dest → Equal(a,b)→RA_JEQ_FLAG; JumpIf→dest
/  MA0=name  MA1=a  MA2=b  MA3=dest  MA4=name_J(auto _J)
/Optimized: no _K NOP. JumpIf's own slot 5 (next, used by interpreter when
/condition is false) IS the fall-through point — autolink leaves RA_MC_PREV
/at name_J, so the next ordinary NOVA in the file links into name_J.next,
/which is exactly where JumpIf jumps when a!=b. No extra lux needed.
NOLINK
/── JEQ name a b dest [_J] ─────────────────────────────────────────────────
/Synthesizes 2 NOVAs: name(Equal a b→FLAG), _J(JumpIf FLAG→dest).
/Explicit next-link: name→_J via slot 5 of name lux.
NOLINK
NOVAE SYNTH_JEQ Cmd=JEQ Op=Equal El1=RA_MA1 El2=RA_MA2 Exit=RA_JEQ_FLAG_PTR At=RA_MA0 Next=RA_MA4

NOVAE SYNTH_JEQ_N2 Op=JumpIf El1=RA_JEQ_FLAG_PTR Exit=RA_MA3 At=RA_MA4

RREDI SYNTH_JEQ_RET
NOLINK

/── JZ name x dest [_J] ─────────────────────────────────────────────────────
/Synthesizes 2 NOVAs: name(Equal x C_0→FLAG), _J(JumpIf FLAG→dest).
NOLINK
NOVAE SYNTH_JZ Cmd=JZ Op=Equal El1=RA_MA1 El2=C_0 Exit=RA_JEQ_FLAG_PTR At=RA_MA0 Next=RA_MA4 Op2=Idem

NOVAE SYNTH_JZ_N2 Op=JumpIf El1=RA_JEQ_FLAG_PTR Exit=RA_MA3 At=RA_MA4

RREDI SYNTH_JZ_RET
NOLINK

//
── GL_IMPL: shared prefix for LX/LH/LR/LT (steps 1-2 of 3) ────
IN:  RA_MA0=name(lux), RA_MA1=src
     RA_MC_SRCREG = ADDRESS of the register src should be staged into
                    (caller sets via Idem -- the address itself, not
                    its current value)
     RA_MC_FUNC   = ADDRESS of the SR_GL* function to call (a plain
                    Move of the function's own name gives its address)
OUT: RA_MC_K = the chain-continuation address the caller's own third
               step should build At=

NOVAE's own "Exit=X" (default Move) resolves to X's CURRENT VALUE at
synthesis time, not X's address -- so "Exit=RA_MC_SRCREG" makes the
generated instruction's real exit slot whatever RA_MC_SRCREG held,
i.e. whichever concrete register the caller pointed it at (Idem gives
that address without dereferencing first). This is what lets one body
serve LX/LH/LR/LT's first two steps: each previously hardcoded its own
input register and its own SR_GL* call as four separate, near-identical
copies (see BUGS.md/HISTORY.md for the session that found this) -- the
caller now supplies those two as data instead.

Only the first two of three steps are shared, not all three: the third
differs in a way Op1/Op2/Op3 (literal text at synth.re's own load time,
not a runtime-settable register like El1/Exit) cannot express as a
parameter -- LX/LH read their result with a plain Move, LR/LT read
theirs with Op1=Idem (a live re-read through a fixed address, since
SR_GLR/SR_GLE write it to a register distinct from the one they read
src from, unlike SR_GLX/SR_GLL). Forcing all four through one shared
third step would have silently changed LR/LT's behavior -- caught
before writing it, not after. Each macro below keeps its own third
step for exactly this reason. LR/LT also keep their own trailing
"Move RA_MC_K -> RA_MC_PREV": AUTOLINK (this file, above) appears to
already do this unconditionally on every NOVAE step regardless of
Op1/Op2/Op3, which would make that line redundant, but this wasn't
confirmed by execution (blocked by the same 'own'-block reservation
gap load_file() has for NOVAE-heavy fragments -- auto_discover()'s
one-time whole-project sizing pass never got a chance to run for an
isolated test); removing an apparently-redundant line without being
able to verify it is exactly the wrong kind of shortcut, so it stays.
//
/N1 is the entry (At=RA_MA0: the caller's name lux). N2 and each caller's
/own N3 below are anonymous continuation nodes -- no At=, so they allocate
/fresh + autolink, uniform with RCALL_AT and LINK_IMPL. The old form threaded
/N2/N3 addresses through RA_MC_J/RA_MC_K, which nothing allocated (they stayed
/0/stale) -- see LINK_IMPL's note.
NOVAE SYNTH_GL_N1 Op=Move El1=RA_MA1 Exit=RA_MC_SRCREG At=RA_MA0 Op3=Idem

NOVAE SYNTH_GL_N2 Op=Voca El1=RA_MC_FUNC Exit=RA_LINK Op3=Idem

RREDI SYNTH_GL_RET
NOLINK
/GL_IMPL is the callable entry point of this shared prefix -- its four
/callers (SYNTH_LX/LH/LR/LT_CALL) do `RVOCA Body=GL_IMPL`, which reads
/a[GL_IMPL] as the jump target. Without this it stayed 0 (an undefined
/forward reference: referenced but never given a body), so the first
/dispatch of LX/LH/LR/LT Voca'd to 0. Dormant until self-hosting's load
/wave actually dispatched an LH (it never ran before the identity/location
/fix). Same entry-alias idiom as SET LOAD_DISPATCH_CORE Value=LOAD_DC_TLENCK.
SET GL_IMPL Value=SYNTH_GL_N1
NOLINK

/── LX lux src dest ──────────────────────────────────────────────────────────
/GLX: src→RA_SR_LUMEN, call SR_GLX, result comes back in RA_SR_LUMEN
/itself (SR_GLX's own last step -- it advances the position and hands
/back the advanced position, not a value read from it, unlike LH/LR/LT
/below; see lumen.re's own SR_GLX for why that's genuinely different
/from "read a field", not just a naming convention).
NOLINK
NOVA SYNTH_LX Cmd=LX Op=Idem El1=RA_SR_LUMEN Exit=RA_MC_SRCREG
NOVA Op=Move El1=SR_GLX Exit=RA_MC_FUNC
RVOCA SYNTH_LX_CALL Body=GL_IMPL
NOVAE SYNTH_LX_N3 Op=Move El1=RA_SR_LUMEN Exit=RA_MA2

RREDI SYNTH_LX_RET
NOLINK

/── LH lux src dest ──────────────────────────────────────────────────────────
/GLL: src→RA_SR_LUX, call SR_GLL, result in RA_SR_LUMEN (SR_FIRST_LM,
/which SR_GLL falls into, writes there -- not the same register it
/read src from, unlike LX above).
NOLINK
NOVA SYNTH_LH Cmd=LH Op=Idem El1=RA_SR_LUX Exit=RA_MC_SRCREG
NOVA Op=Move El1=SR_GLL Exit=RA_MC_FUNC
RVOCA SYNTH_LH_CALL Body=GL_IMPL
NOVAE SYNTH_LH_N3 Op=Move El1=RA_SR_LUMEN Exit=RA_MA2

RREDI SYNTH_LH_RET
NOLINK

//
── WALK_ONE lux rel dest — separate from WALK_NOVA, not a duplicate ──
WALK_ONE walks one generic lumen-chain step (data structures: rings,
tethers). WALK_NOVA specifically walks a NOVA instruction's own pad
slot (offset=C_6=SLOT_PAD): 0 there means no extra lumens (WALK_ONE
finds nothing, same as an empty Data lux); non-zero means the extra
lumens start right at that slot -- a
different job, not the same thing written twice; see lumen.re's own
note on WALK_NOVA. They also place the caller-provided "name" lux on
different steps (WALK_ONE: last/Voca step, so re-invoking "name" later
re-runs just the SR_WALK_ONE call against whatever RA_SR_LUX/REL/
OFFSET setup already happened -- useful for repeated single-step
advances like a ring; WALK_NOVA: first step, so re-invoking always
redoes the full sequence) -- this looked at first like it should be
unified away, but "At=" (like Op1/Op2/Op3) is literal text at this
file's own load time, not a runtime-settable register, so which step
gets the caller's name has to be chosen in the source text of each,
not passed as a parameter. Kept separate for this reason -- a real
difference, not an accidental copy (see BUGS.md/HISTORY.md for the
session that almost merged these before catching it).

WALK_ONE's own offset step (below) is the one fix carried over from
that near-merge: it used to write "Move El1=C_1 Exit=RA_SR_OFFSET"
with no Op1 override. Op1 defaults to Move, so the SYNTHESIZED
instruction's own El1 field ended up holding C_1's VALUE (1) rather
than C_1's ADDRESS -- meaning the built instruction, when it later
ran, read "whatever's stored at address 1" instead of writing the
literal constant 1 into RA_SR_OFFSET. WALK_NOVA's own analogous step
already had Op1=Idem (correctly using C_7's address), which is what
exposed the asymmetry.
//
NOLINK
/N1..N3 are anonymous setup nodes -> no At= (fresh + autolink); they chain
/N1->N2->N3->N4. N4 is the named, re-invokable step (At=RA_MA0). The old
/RA_MA3/4/5 carriers were never provided (every WALK_ONE usage passes just
/name + 2 args) -- the same stale-carrier bug as ALLOC_TO/LINK.
NOVAE SYNTH_WALK_ONE Cmd=WALK_ONE Op=Move El1=RA_MA1 Exit=RA_SR_LUX

NOVAE SYNTH_WALK_ONE_N2 Op=Move El1=RA_MA2 Exit=RA_SR_REL

NOVAE SYNTH_WALK_ONE_N3 Op=Move El1=C_1 Exit=RA_SR_OFFSET Op1=Idem

NOVAE SYNTH_WALK_ONE_N4 Op=Voca El1=SR_WALK_ONE Exit=RA_LINK At=RA_MA0 Op3=Idem

RREDI SYNTH_WALK_ONE_RET
NOLINK

NOLINK

//
── LINK_IMPL: shared body for LINK_OP/UNLINK_OP ───────────────
IN:  RA_MA0=name(lux), RA_MA1=src, RA_MA2=rel, RA_MA3=tgt
     RA_MC_LUMENOP = address of the lumen-op subroutine to call
                     (ADD_LUMEN or REMOVE_LUMEN -- caller sets via a
                     plain Move, same self-ref convention every other
                     Voca target in this file already relies on)
     RA_LM_OFFSET = lux-kind offset (caller determines via NOVA_ADDRS_CHECK)
Synthesizes 5 NOVAs: lux(Move src→LM_SRC), _J(Move rel→LM_REL),
_K(Move tgt→LM_EXIT), _F(Move offset→LM_OFFSET), _L(Voca *LUMENOP).
//
/N1 is the entry (At=RA_MA0: reuse the caller's name lux). N2..N4 are
/anonymous continuation nodes -- no At=, so each allocates a fresh lux at
/runtime and autolinks to the previous, exactly like RCALL_AT's P2..P6. The
/old form threaded addresses through RA_MC_J/K/FLAG/LUX, but nothing ever
/allocated those and RA_MC_LUX/RA_MC_FLAG are the emit's OWN working
/registers (At= writes RA_MC_LUX; AUTOLINK clobbers RA_MC_FLAG) -- so N2/N3
/emitted at 0 and N_OFF/N4 at stale register addresses, scribbling low memory.
NOVAE SYNTH_LI_N1 Op=Move El1=RA_MA1 Exit=RA_LM_SRC At=RA_MA0 Op3=Idem

NOVAE SYNTH_LI_N2 Op=Move El1=RA_MA2 Exit=RA_LM_REL Op3=Idem

NOVAE SYNTH_LI_N3 Op=Move El1=RA_MA3 Exit=RA_LM_EXIT Op3=Idem

NOVAE SYNTH_LI_N_OFF Op=Move El1=RA_LM_OFFSET Exit=RA_LM_OFFSET Op3=Idem

NOVAE SYNTH_LI_N4 Op=Voca El1=RA_MC_LUMENOP Exit=RA_LINK Op3=Idem

RREDI SYNTH_LI_RET
NOLINK
/LINK_IMPL is the callable entry of this shared body (its callers do
/RVOCA Body=LINK_IMPL, which reads a[LINK_IMPL] as the target). Without
/this alias it stayed 0 -- undefined call target, same latent class as
/GL_IMPL -- and would fault the first time LINK_OP/UNLINK_OP dispatched in
/the load wave. Entry-alias idiom (cf. SET GL_IMPL Value=SYNTH_GL_N1).
SET LINK_IMPL Value=SYNTH_LI_N1
NOLINK

/── LINK_OP lux src rel tgt ─────────────────────────────────────────────────
NOLINK
NOVA SYNTH_LINK_OP Cmd=LINK_OP Op=Move El1=ADD_LUMEN Exit=RA_MC_LUMENOP
NOVA SYNTH_LO_MVSRC Op=Move El1=RA_MA1 Exit=RA_LM_SRC
RVOCA SYNTH_LO_NAC Body=NOVA_ADDRS_CHECK
RVOCA SYNTH_LINK_OP_CALL Body=LINK_IMPL
RREDI SYNTH_LINK_OP_RET
NOLINK

/── LR lux src dest ─────────────────────────────────────────────────────────
/GLR: src→RA_SR_LUMEN, call SR_GLR, result read back via Op1=Idem (live
/re-read of RA_SR_REL -- SR_GLR writes there, a different register than
/the one it read src from).
NOLINK
NOVA SYNTH_LR Cmd=LR Op=Idem El1=RA_SR_LUMEN Exit=RA_MC_SRCREG
NOVA Op=Move El1=SR_GLR Exit=RA_MC_FUNC
RVOCA SYNTH_LR_CALL Body=GL_IMPL
NOVAE SYNTH_LR_N3 Op=Move El1=RA_SR_REL Exit=RA_MA2 Op1=Idem

RREDI SYNTH_LR_RET
NOLINK

/── LT lux src dest ─────────────────────────────────────────────────────────
/GLE: src→RA_SR_LUMEN, call SR_GLE, result read back via Op1=Idem (live
/re-read of RA_SR_OUT).
NOLINK
NOVA SYNTH_LT Cmd=LT Op=Idem El1=RA_SR_LUMEN Exit=RA_MC_SRCREG
NOVA Op=Move El1=SR_GLE Exit=RA_MC_FUNC
RVOCA SYNTH_LT_CALL Body=GL_IMPL
NOVAE SYNTH_LT_N3 Op=Move El1=RA_SR_OUT Exit=RA_MA2 Op1=Idem

RREDI SYNTH_LT_RET
NOLINK

/── WALK_NOVA lux src rel dest MA3 MA4 MA5 MA6 ───────────────────────────────
/Kept separate from WALK_ONE -- see that macro's own header comment
/above for why (different job: stepping to a NOVA's pad slot, offset=
/C_6=SLOT_PAD, vs a generic lumen-chain step; and a real, load-time-only
/"At=" placement difference that can't be parameterized). Offset is
/C_6, not C_7/NOVA_SIZE: extra lumens, when present, start right at the
/pad slot itself now (it doubles as their first entry), not after a
/separate always-empty slot -- see constants.re's Flux/NOVA layout
/notes and interpreter.py's _nova_extra_skip. Confirmed unused anywhere
/in the project currently (grep found no caller), but kept and given
/the same consideration as WALK_ONE regardless -- unreachable now
/doesn't mean wrong, and this one may be exactly the counterpart
/WALK_ONE's own design was built expecting to have.
NOLINK
/N1 is the entry (At=RA_MA0). N2..N4 anonymous -> no At= (fresh + autolink),
/same as LINK/GL; the old RA_MC_J/K/LUX carriers were never allocated.
NOVAE SYNTH_WALK_NOVA Cmd=WALK_NOVA Op=Move El1=RA_MA1 Exit=RA_SR_LUX At=RA_MA0 Op3=Idem

NOVAE SYNTH_WALK_NOVA_N2 Op=Move El1=RA_MA2 Exit=RA_SR_REL Op3=Idem

NOVAE SYNTH_WALK_NOVA_N3 Op=Move El1=C_6 Exit=RA_SR_OFFSET Op1=Idem Op3=Idem

NOVAE SYNTH_WALK_NOVA_N4 Op=Voca El1=SR_WALK_ONE Exit=RA_LINK Op3=Idem

RREDI SYNTH_WALK_NOVA_RET
NOLINK

/── UNLINK_OP lux src rel tgt ───────────────────────────────────────────────
NOLINK
NOVA SYNTH_UNLINK_OP Cmd=UNLINK_OP Op=Move El1=REMOVE_LUMEN Exit=RA_MC_LUMENOP
NOVA SYNTH_UO_MVSRC Op=Move El1=RA_MA1 Exit=RA_LM_SRC
RVOCA SYNTH_UO_NAC Body=NOVA_ADDRS_CHECK
RVOCA SYNTH_UNLINK_CALL Body=LINK_IMPL
RREDI SYNTH_UNLINK_OP_RET
NOLINK

/── RCALL_AT name sub landing ──────────────────────────────────────────────
/Synthesizes a 7-NOVA continuation block at runtime:
/  name:    Move RA_SP → RA_RCA_TMP          (save stack pointer)
/  name_P2: Sub  RA_RCA_TMP, FRAME_SIZE      (allocate frame)
/  name_P3: Move RA_RCA_TMP → RA_SP          (update stack pointer)
/  name_P4: Move RA_LINK → RA_RCA_TMP2       (save return address)
/  name_P5: Write RA_RCA_TMP, RA_RCA_TMP2    (push return addr onto stack)
/  name_P6: Move landing → RA_LINK           (redirect return to landing)
/  name_J:  Jump Exit=sub                    (call the subroutine)
/At=RA_MA0 on first NOVAE: reuse the caller-provided name lux as entry point
/instead of allocating a new one. All subsequent NOVAEs allocate fresh luces.
NOLINK
NOVAE SYNTH_RCALL_AT Cmd=RCALL_AT Op=Move El1=RA_SP Exit=RA_RCA_TMP At=RA_MA0 Op1=Idem Op3=Idem

NOVAE SYNTH_RCALL_AT_P2 Op=Sub El1=RA_RCA_TMP El2=RA_FRAME_SIZE Exit=RA_RCA_TMP Op1=Idem Op2=Idem Op3=Idem

NOVAE SYNTH_RCALL_AT_P3 Op=Move El1=RA_RCA_TMP Exit=RA_SP Op1=Idem Op3=Idem

NOVAE SYNTH_RCALL_AT_P4 Op=Move El1=RA_LINK Exit=RA_RCA_TMP2 Op1=Idem Op3=Idem

NOVAE SYNTH_RCALL_AT_P5 Op=Write El1=RA_RCA_TMP El2=RA_RCA_TMP2 Op1=Idem Op2=Idem

NOVAE SYNTH_RCALL_AT_P6 Op=Move El1=RA_MA2 Exit=RA_LINK Op3=Idem
/name_J: Jump to sub (chain terminator). Anonymous -> no At=, so it allocates
/fresh and autolinks onto name_P6 (the manual RA_MC_J next-wire is gone -- that
/register was never allocated, same latent bug as LINK/GL). Reset=1 makes
/AUTOLINK reset RA_MC_PREV after linking (chain terminator), via WRITE_NOVA_SLOTS_RESET.
NOVAE SYNTH_RCALL_AT_J Op=Jump Exit=RA_MA1 Reset=1

RREDI SYNTH_RCALL_AT_RET
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
/    RA_MC_DEST=reg addr, via Idem (RA_TW_LUX/RA_TMP2/RA_BYTE).
/Wires: name(Move arg→reg_ref) → anon_J(Voca fn Exit=RA_LINK). Sets RA_MC_PREV=anon_J.
/Optimized: no anon_K NOP. Voca saves nxt (anon_J's slot 5) into RA_LINK on
/call; when fn returns via Redi(RA_LINK), execution resumes exactly at
/anon_J's slot 5 — the same place autolink would link the next ordinary
/NOVA into. So leaving RA_MC_PREV=anon_J after this macro makes the next
/file line continue correctly without an extra NOP lux.
SET RCN_IMPL Value=SYNTH_RCN_S0
/── RCN_IMPL: shared body for EMIT/EMITI/PUTBYTE ───────────────────────────
/Synthesizes 2 NOVAs:
/  name(Move RA_MA1→RA_MC_DEST) — written via direct slot-writes + AUTOLINK
/  _J  (Voca RA_MC_OP)          — synthesized via NOVAE At=RA_MC_J, Next=linked
NOLINK
/First NOVA: write slots directly (Move arg→reg_ref), then AUTOLINK
NOVA SYNTH_RCN_S0 Op=Move El1=RA_MC_LUX Exit=RA_MC_SLOT
NOVA SYNTH_RCN_W0 Op=Write El1=RA_MC_SLOT El2=RA_MC_LUX
NOVA SYNTH_RCN_A1 Op=Add El1=RA_MC_LUX El2=C_1 Exit=RA_MC_SLOT
NOVA SYNTH_RCN_W1 Op=Write El1=RA_MC_SLOT El2=Move
NOVA SYNTH_RCN_A2 Op=Add El1=RA_MC_LUX El2=C_2 Exit=RA_MC_SLOT
NOVA SYNTH_RCN_W2 Op=Write El1=RA_MC_SLOT El2=RA_MA1
NOVA SYNTH_RCN_A4 Op=Add El1=RA_MC_LUX El2=C_4 Exit=RA_MC_SLOT
NOVA SYNTH_RCN_W4 Op=Write El1=RA_MC_SLOT El2=RA_MC_DEST
NOVA SYNTH_RCN_CAL Op=Voca El1=AUTOLINK Exit=RA_LINK
/Second NOVA: Voca RA_MC_OP. Anonymous _J -> no At=, allocates fresh and
/autolinks onto the first NOVA (whose AUTOLINK left RA_MC_PREV = the name lux).
/The manual RA_MC_J next-wire and the trailing RA_MC_J->RA_MC_PREV are gone:
/RA_MC_J was never allocated (same bug as LINK/GL), and AUTOLINK now leaves
/RA_MC_PREV = this _J for the next file line -- exactly what the header note wants.
NOVAE SYNTH_RCN_J Op=Voca El1=RA_MC_OP Exit=RA_LINK Op3=Idem

RREDI SYNTH_RCN_RET
/── EMIT name arg → Move arg→RA_TW_LUX; Voca EMIT_STR_ENTRY; NOP
/MA0=name_addr MA1=arg_addr
NOLINK
NOVA SYNTH_EMIT Cmd=EMIT Op=Move El1=RA_MA0 Exit=RA_MC_LUX
NOVA SYNTH_EMIT_OP Op=Move El1=EMIT_STR_ENTRY Exit=RA_MC_OP
NOVA SYNTH_EMIT_DST Op=Idem El1=RA_TW_LUX Exit=RA_MC_DEST
RVOCA SYNTH_EMIT_DO Body=RCN_IMPL
RREDI SYNTH_EMIT_RET
/── EMITI name arg → Move arg→RA_TMP2; Voca EMIT_INT_ENTRY; NOP
/MA0=name_addr MA1=arg_addr
NOLINK
NOVA SYNTH_EMITI Cmd=EMITI Op=Move El1=RA_MA0 Exit=RA_MC_LUX
NOVA SYNTH_EMITI_OP Op=Move El1=EMIT_INT_ENTRY Exit=RA_MC_OP
NOVA SYNTH_EMITI_DST Op=Idem El1=RA_TMP2 Exit=RA_MC_DEST
RVOCA SYNTH_EMITI_DO Body=RCN_IMPL
RREDI SYNTH_EMITI_RET
/── PUTBYTE name arg → Move arg→RA_BYTE; Voca PUT_BYTE; NOP
/MA0=name_addr MA1=arg_addr
NOLINK
NOVA SYNTH_PUTBYTE Cmd=PUTBYTE Op=Move El1=RA_MA0 Exit=RA_MC_LUX
NOVA SYNTH_PUTBYTE_OP Op=Move El1=PUT_BYTE Exit=RA_MC_OP
NOVA SYNTH_PUTBYTE_DST Op=Idem El1=RA_BYTE Exit=RA_MC_DEST
RVOCA SYNTH_PUTBYTE_DO Body=RCN_IMPL
RREDI SYNTH_PUTBYTE_RET

//
────────────────────────────────────────────────────────────────
Below: SYNTH_YAKU, moved here from macros.re. YAKU has no Python-side
handling anywhere (not in _SPECIAL_OPENERS, no cmd=="YAKU" in
_wave2_wire_line) -- shared, runtime-callable, reached identically by
cold-boot's generic dispatch and by PS_MAIN. Same category as
RCALL_AT/WALK_ONE above (SPAN, formerly here too, was deleted --
absorbed by NEW's Count=/Width= long before this file existed; see
BUGS.md/HISTORY.md for the session that found the leftover and
removed it).
────────────────────────────────────────────────────────────────
//

/── YAKU: build a packed string template and optionally link to Yaku ──────────
/YAKU name [relation] [alias]
/  body lines (indented)...
/
/  name     — lux to store the packed string in (MA0)
/  relation — optional: if given, LINK Yaku_name --relation--> Yaku  (MA1, 0=absent)
/  alias    — optional: if given, also LINK Yaku_name --ForType--> alias (MA2, 0=absent)
/
/Replaces: YAKU_NEXO, YAKU_NEXO_TERM, YAKU_NEXO_CMP, YAKU_NEXO_ARITH, YAKU_NEXO_ALIAS.
/Examples:
/  YAKU Add HasArithResult     ← was YAKU_NEXO_ARITH Add
/  YAKU End Terminates         ← was YAKU_NEXO_TERM End
/  YAKU Equal HasCmpResult     ← was YAKU_NEXO_CMP Equal
/  YAKU Read Terminates Write  ← was YAKU_NEXO_ALIAS Read (alias=Write)
/  YAKU Move                   ← was YAKU_NEXO Move (no relation, inline or body)
/Reached via wax.re's LOAD_CMD_UNKNOWN generic "builder macro" dispatch
/(reads args via LOAD_READARG_KV, then Voca's to whatever this word
/points at) -- NOT via parser.re's separate PS_LINE/PS_MAIN system, which
/is unrelated to LOAD_MAIN's real self-hosting dispatch (see BUGS.md
/Pattern Y for the full history of this correction).
NOLINK
/Init packed string builder
NOVA SYNTH_YAKU Cmd=YAKU Op=Move El1=C_0 Exit=PGB_PS_FIRST
NOVA Op=Move El1=C_0 Exit=PGB_PS_WORD
NOVA Op=Move El1=C_0 Exit=PGB_PS_SHIFT

/Push YAKU context: saves enclosing indent state, reinits indent stack
/rooted at this header's indent (SK_IND_COUNT from LOAD_COUNT_INDENT).
NOVA SYNTH_YK_CMODE Op=Move El1=CTX_MODE_YAKU Exit=SK_TMP3
RVOCA SYNTH_YK_CPUSH Body=LOAD_CTX_PUSH
RVOCA SYNTH_YK_SKP Body=BS_SKIP_TO_EOL
/Read body lines until non-indented or EOF
RVOCA SYNTH_YK_LINE_LOOP Body=LOAD_READ_LINE
/EOF or non-indented → done
JEQ SYNTH_YK_EOFCK El1=RA_LOAD_BYTE El2=C_0 Exit=SYNTH_YK_DONE_BUILD
/Classify the exact indent LOAD_READ_LINE just measured, so SK_IND_DEPTH
/is populated for the check below.
RVOCA SYNTH_YK_MARKC Body=LOAD_INDENT_MARK
JEQ SYNTH_YK_INDCK El1=SK_IND_DEPTH El2=C_0 Exit=SYNTH_YK_DONE_BUILD
/Emit LF between lines (except first)
JZ SYNTH_YK_LFCK El1=PGB_PS_FIRST Exit=SYNTH_YK_READ_LINE_BYTES
NOVA SYNTH_YK_LF_EMIT Op=Move El1=LF Exit=RA_TMP
RVOCA SYNTH_YK_LFEB Body=PGB_EMIT_BYTE
/Read and emit bytes of this line
RVOCA SYNTH_YK_READ_LINE_BYTES Body=BS_READ_BYTE
JEQ SYNTH_YK_BEOF El1=RA_LOAD_BYTE El2=C_0 Exit=SYNTH_YK_LINE_LOOP
JEQ SYNTH_YK_BLF El1=RA_LOAD_BYTE El2=LF Exit=SYNTH_YK_LINE_LOOP
/Check for '{'
JEQ SYNTH_YK_LBCK El1=RA_LOAD_BYTE El2=LBRACE Exit=SYNTH_YK_PLACEHOLDER
/Ordinary byte
NOVA SYNTH_YK_EBYTE Op=Move El1=RA_LOAD_BYTE Exit=RA_TMP
RVOCA SYNTH_YK_EB Body=PGB_EMIT_BYTE
NOVA SYNTH_YK_BJMP Op=Jump Exit=SYNTH_YK_READ_LINE_BYTES
/Placeholder: read name bytes into tokbuf, call PGB_PLACEHOLDER
NOVA SYNTH_YK_PLACEHOLDER Op=Move El1=C_0 Exit=PR_LPOS
NOVA Op=Move El1=BS_TOKBUF_BASE Exit=PR_LBUF

RVOCA SYNTH_YK_PL_RBYTE Body=BS_READ_BYTE
JEQ SYNTH_YK_PL_RBCK El1=RA_LOAD_BYTE El2=RBRACE Exit=SYNTH_YK_PL_CALL
NOVA SYNTH_YK_PL_STORE Op=Add El1=BS_TOKBUF_BASE El2=PR_LPOS Exit=RA_BS_TMP
NOVA SYNTH_YK_PL_SW Op=Write El1=RA_BS_TMP El2=RA_LOAD_BYTE
NOVA SYNTH_YK_PL_INC Op=Add El1=PR_LPOS El2=C_1 Exit=PR_LPOS
NOVA SYNTH_YK_PL_JMP Op=Jump Exit=SYNTH_YK_PL_RBYTE
NOVA SYNTH_YK_PL_CALL Op=Move El1=PR_LPOS Exit=PR_LLEN
RVOCA SYNTH_YK_PL_PHCALL Body=PGB_PLACEHOLDER
NOVA SYNTH_YK_PL_BJMP Op=Jump Exit=SYNTH_YK_READ_LINE_BYTES
/Done: finalize + write packed string into MA0.word
NOLINK
/Pop YAKU context: restores enclosing indent state.
/Both exit JEQs (EOFCK, INDCK) jump here, so this label replaces
/the old SYNTH_YK_DONE_BUILD as the body-exit target.
RVOCA SYNTH_YK_DONE_BUILD Body=LOAD_CTX_POP
RVOCA SYNTH_YK_FINALIZE Body=PGB_FINALIZE
NOVA SYNTH_YK_WRITE Op=Write El1=RA_MA0 El2=PGB_PS_FIRST
/If MA1 (relation) given: LINK MA0 --MA1--> Yaku
JZ SYNTH_YK_RELCK El1=RA_MA1 Exit=SYNTH_YK_ALIAS_CK
NOVA SYNTH_YK_REL_SRC Op=Move El1=RA_MA0 Exit=RA_LM_SRC
NOVA Op=Move El1=RA_MA1 Exit=RA_LM_REL
NOVA Op=Move El1=Yaku Exit=RA_LM_EXIT
NOVA SYNTH_YK_LOFF Op=Move El1=NOVA_SIZE Exit=RA_LM_OFFSET
RVOCA SYNTH_YK_REL_ADD Body=ADD_LUMEN
/If MA2 (alias) given: LINK MA0 --ForType--> MA2
JZ SYNTH_YK_ALIAS_CK El1=RA_MA2 Exit=SYNTH_YK_NEXO_DONE
NOVA SYNTH_YK_ALIAS_SRC Op=Move El1=RA_MA0 Exit=RA_LM_SRC
NOVA Op=Move El1=ForType Exit=RA_LM_REL
NOVA Op=Move El1=RA_MA2 Exit=RA_LM_EXIT

RVOCA SYNTH_YK_ALIAS_ADD Body=ADD_LUMEN
RREDI SYNTH_YK_NEXO_DONE



════════ PART 3 -- was parser.re (NEW / SET / LINK / NOVA command handlers) ════════
/These are WAX_CMD-dispatched builder handlers like everything else above.
/They reference parser-owned state (PR_EL1/PR_SYM/SK_PLINK_PTR/SK_NOVA_*)
/and shared infra (LOAD_ALLOC_SIZED, LOAD_LCOUNT_GET, PS_INS, ADD_LUMEN,
/WRITE_NOVA_SLOTS) by name -- all cross-file, addresses don't care which
/file a label sits in. LOAD stays in parser.re: its entry (PS_LINE_LOAD)
/is a label inside PS_LINE, the parser's own dispatch loop, not a builder.
============================================================
/PS_CMD_NEW — create a new Lux, insert into BS_HTAB
/Idempotent: mirrors wax.re's LOAD_CMD_NEW. If the name already exists
/(found via PS_LK), this is a no-op — PR_EL1 is set to the EXISTING Lux
/and no new allocation happens. Before this fix, PS_CMD_NEW always
/allocated unconditionally: a second `NEW X` in the same parse created a
/second, orphaned Lux (PR_EL1 silently rebinding BS_HTAB's "X" to the new
/one, the original lost but still occupying memory) — same class of bug
/LOAD_CMD_NEW was already written to avoid. Fixed to match.
============================================================
/The name comes from the generic pre-read (LOAD_READARG_KV): the first
/keyless token lands in MA0, already resolved to its address -- the alloc
/wave (loader.py's _wave2_alloc_line, mirrored here) has minted every
/NEW/SET/NOVA name before any handler wires it, so MA0 is that address,
/not a fresh lookup of our own. No self-read (BS_READ_TOKEN) and no PS_LK
/here anymore -- both were a private second copy of what the pre-read
/already did. MA0==0 is the never-in-a-multi-wave-run fallback (a name
/that resolved to nothing), still routed to the allocate path below.
NOVA PS_CMD_NEW Op=Move El1=RA_MA0 Exit=PR_SYM Cmd=NEW
JZ PS_NW_EXCK El1=PR_SYM Exit=PS_NW_ALLOC
/already exists: PR_SYM is the existing Lux — use it, allocate nothing
NOVA PS_NW_REUSE Op=Move El1=PR_SYM Exit=PR_EL1
RREDI RET_NW_REUSE
NOLINK
/Size the allocation to hold this name's future LINK lumina, not a flat
/two luces: 1 + 2*lcount + (lcount>0 ? 1 : 0) -- word, one pair per
/lumen, and a sentinel when there is at least one. The count comes from
/the prepass, via LOAD_LCOUNT_GET. This sizing used to live in wax.re's
/own LOAD_CMD_NEW, which was the second implementation of this command;
/that one is gone now and this is the only one, so the sizing had to
/come with it -- a name allocated flat has nowhere to put its lumina
/later, and nothing would report the loss until a LINK went missing.
RVOCA PS_NW_ALLOC Body=LOAD_LCOUNT_GET
/Base=1 (plain data lux), terminator always forced -- a data lux's own
/base slot has no built-in pad the way NOVA's slot 6 does, so unlike
/NOVA it needs the +1 terminator even when lcount is 0. This also fixes
/a real bug in the inline calc formerly here: it used the CONDITIONAL
/(NOVA-shaped) terminator check by copy-paste, under-allocating every
/zero-lumen name by 1 slot.
NOVA PS_NW_ABASE Op=Move El1=C_1 Exit=RA_ALLOC_BASE
NOVA PS_NW_ATFORCE Op=Move El1=C_1 Exit=RA_ALLOC_TERM_FORCE
RVOCA PS_NW_DOALLOC Body=LOAD_ALLOC_SIZED
NOVA PS_NW_SETEL1 Op=Move El1=RA_ALLOC_RESULT Exit=PR_EL1
RVOCA CG_NW_INS Body=PS_INS
RREDI RET_NW_INS_JR

============================================================
//
PS_CMD_SET — SET name value  (SET [Name=]X Value=V | SET X V | bare SET X)
Reads entirely from the pre-read: name = MA0, value = SK_ARG_Value with
its kind in the adjacent slot (SK_ARG_Value+1). int -> store the number, name/string -> store
the address, bare -> self-ref. String values are packed by the reader
(LOAD_PACK_STRING) before they get here, so there is no private string
byte-loop and no BS_PEEK_BYTE peek anymore -- one uniform path, mirroring
loader.py's _wire_word_value. The value/reference distinction rides on
SK_ELEM_KIND (see its declaration): the one bit eager address-resolution
would otherwise lose.
//
/============================================================

/All arguments come from the generic pre-read (LOAD_READARG_KV) now -- no
/self-read, no PS_LK, no BS_PEEK_BYTE, no private string byte-loop. The
/name is the first keyless token (MA0, resolved to the target's address);
/the value is SK_ARG_Value with its kind in the adjacent slot (SK_ARG_Value+1). A "..." value
/was already packed to a chain address by the reader (LOAD_PACK_STRING),
/so string, name and int all arrive here uniformly -- only how the word is
/stored differs, exactly as loader.py's _wire_word_value decides it.
NOVA PS_CMD_SET Op=Move El1=RA_MA0 Exit=PR_EL1 Cmd=SET
/Bare `SET X` (no Value=) -> self-ref word(X)=addr(X). The reader clears
/SK_ARG_Value to 0 each line, so 0 means "no value given".
JZ PS_ST_BARECK El1=SK_ARG_Value Exit=PS_ST_BARE
/Number literal (kind 1) -> store the NUMBER: read it out of its constant
/cell. Name/string (kind 0) -> store the address as-is. The kind now lives in
/the value slot's adjacent cell (SK_ARG_Value+1), written there by the generic
/keyed store -- no separate SK_ARG_ValueK register any more.
NOVA PS_ST_KA Op=Idem El1=SK_ARG_Value Exit=SK_TMP
NOVA PS_ST_KA1 Op=Add El1=SK_TMP El2=C_1 Exit=SK_TMP
NOVA PS_ST_KR Op=Read El1=SK_TMP Exit=SK_TMP
JEQ PS_ST_KINDCK El1=SK_TMP El2=C_1 Exit=PS_ST_INT
NOVA PS_ST_REFWR Op=Write El1=PR_EL1 El2=SK_ARG_Value
RREDI PS_ST_REFRET
NOLINK
NOVA PS_ST_BARE Op=Write El1=PR_EL1 El2=PR_EL1
RREDI PS_ST_BARERET
NOLINK
/Integer: pull the value out of the constant cell (Read = double deref)
/and write it -- nothing else, exactly as loader.py's _wire_word_value.
/No Constant lumen: Python records none and nothing reads one, and the
/target here is a 1-cell interned name with no lumen room -- ADD_LUMEN
/walked past it into the neighbouring name cells, planting word=Yaku in
/them, and EXP_CMD_NOVA then "reused" Yaku as their body (overwrote
/SYNTH_YK_LOFF, Yaku's physical neighbour).
NOVA PS_ST_INT Op=Read El1=SK_ARG_Value Exit=SK_TMP
NOVA PS_ST_WR Op=Write El1=PR_EL1 El2=SK_TMP
RREDI PS_ST_RJ


============================================================
/PS_CMD_LINK — LINK src rel exit
============================================================
/Operands come from the generic pre-read (LOAD_READARG_KV): the name
/is the first keyless token (MA0), rel and exit the next two (MA1, MA2),
/each already resolved to its address by the shared reader. No self-read
/(BS_READ_TOKEN) and no own lookup (PS_LK/PS_NEXT_SYMBOL) here anymore --
/that was a second, private copy of the exact token-read-and-classify
/the pre-read already runs once for every command that reaches dispatch.
NOVA PS_CMD_LINK Op=Move El1=RA_MA0 Exit=PR_EL1 Cmd=LINK
NOVA RET_LN_LK1_MV Op=Move El1=RA_MA1 Exit=PR_EL2
NOVA RET_LN_NS2 Op=Move El1=RA_MA2 Exit=PR_EL3
/Record the link instead of applying it here. Applying immediately only
/works when the target is already declared, and when reading a file top
/to bottom it usually is not: "LINK A rel B" routinely appears before B
/exists. The triple goes to the pending buffer and is applied by
/LOAD_APPLY_LINKS once every name's address and lux-kind prefix are
/known. This came across from wax.re's LOAD_CMD_LINK_IMPL, which was
/the second implementation of this command. It is not a build-time
/special case -- forward references are a property of reading text, so
/this is simply what LINK has to do.
NOVA PS_LN_W0 Op=Write El1=SK_PLINK_PTR El2=PR_EL1
NOVA PS_LN_A1 Op=Add El1=SK_PLINK_PTR El2=C_1 Exit=PR_TMP
NOVA PS_LN_W1 Op=Write El1=PR_TMP El2=PR_EL2
NOVA PS_LN_A2 Op=Add El1=SK_PLINK_PTR El2=C_2 Exit=PR_TMP
NOVA PS_LN_W2 Op=Write El1=PR_TMP El2=PR_EL3
NOVA PS_LN_ADV Op=Add El1=SK_PLINK_PTR El2=C_3 Exit=SK_PLINK_PTR
NOVA PS_LN_SEN Op=Write El1=SK_PLINK_PTR El2=C_0
RREDI PS_LN_RJ

/PS_CMD_INSTR and PS_LK_SLOT removed -- both replaced by EXP_CMD_NOVA
/and LOAD_NOVA_READ_ARGS below, physically moved here from expand.re
/(this file owns grammar parsing per its own header; that's where the
/logic belongs, wax.re should never have grown its own second copy).
/PS_CMD_INSTR was the older, buggier implementation: name-by-position
/(first token = name, unconditionally) instead of name-by-exclusion,
/no Body/Count/Value/... support, and PS_LK_SLOT hardcoded raw key
/numbers (C_11, C_1, C_2, C_3) instead of routing by the discovered key
/vocabulary the way LOAD_ELEM/KEY_HTAB now does. PS_LINE_INS (below) now
/calls EXP_CMD_NOVA directly.

── EXP_CMD_NOVA: NOVA name Op=op [El1=x] [El2=y] [Exit=z] ─
//
Every argument comes from the generic pre-read (LOAD_READARG_KV) now,
the same reader every pre-read command shares -- not a private front-end
of this command's own. The name is the first keyless token (MA0,
resolved to its symbol cell, or 0 when the line is anonymous); the op is
SK_ARG_Op; El1/El2/Exit are SK_ARG_El1/El2/Exit -- each already read and
classified exactly once by the reader.

This replaced ~160 lines that re-did all of that by hand: BS_READ_TOKEN
of token0/token1, the "first token without a key is the name" exclusion,
op-token stashing/unstashing, and a private LOAD_NOVA_READ_ARGS loop over
El1=/El2=/Exit= -- a second, complete copy of the reader living inside
one command. Name-by-exclusion is exactly what the reader's "first
keyless token -> MA0" rule already does; keeping a hand copy here was the
duplication this alignment removes. What remains is only this command's
own real work: decide new-vs-existing, size and mint (or reuse) the lux,
and wire it. Alloc NOVA_SIZE + 2*lcount + (lcount>0?1:0) luces (anonymous:
just NOVA_SIZE); reuses an existing lux when the name already carries a
body; autolinks from RA_MC_PREV via WRITE_NOVA_SLOTS.

In the multi-wave model the name is already minted by the alloc wave
before this handler wires it, so the new-alloc/size path here is the
never-taken-in-a-real-run fallback (word(name cell) == 0), kept whole so
nothing is lost -- not deleted, only fed from registers instead of tokens.
//
NOLINK
NOVA EXP_CMD_NOVA Op=Move El1=SK_ARG_Op Exit=RA_BS_RESULT Cmd=NOVA
NOVA EXP_CI_E1IN Op=Move El1=SK_ARG_El1 Exit=SK_NOVA_E1
NOVA EXP_CI_E2IN Op=Move El1=SK_ARG_El2 Exit=SK_NOVA_E2
NOVA EXP_CI_EXIN Op=Move El1=SK_ARG_Exit Exit=SK_NOVA_EXIT
CLEAR EXP_CI_ANON0 El1=SK_CI_ANON
/Anonymous when the line carried no keyless token (MA0 == 0).
JZ EXP_CI_ANONCK El1=RA_MA0 Exit=EXP_CI_ANON_ALLOC
/Named: the name's own cell is MA0 (the reader already interned it), and
/its saved hash (SK_MA_HASH0) drives the lcount lookup -- no re-read and
/no re-intern of our own.
NOVA EXP_CI_HASHR Op=Move El1=SK_MA_HASH0 Exit=RA_LOAD_HASH
RVOCA EXP_CI_LCGET Body=LOAD_LCOUNT_GET
NOVA EXP_CI_LCSAVE Op=Move El1=SK_TMP Exit=SK_NOVA_LCNT
/word(name cell) already set = reuse that body; 0 = mint one below.
NOVA EXP_CI_EWGET Op=Read El1=RA_MA0 Exit=SK_NOVA_ADDR
JZ EXP_CI_EWCK El1=SK_NOVA_ADDR Exit=EXP_CI_NEWSIZE
NOVA EXP_CI_EXJMP Op=Jump Exit=EXP_CI_WIRE
/Anonymous entry: nothing to size or register, count 0, mark it.
NOLINK
CLEAR EXP_CI_ANON_ALLOC El1=SK_NOVA_LCNT
NOVA EXP_CI_ANON_SET Op=Move El1=C_1 Exit=SK_CI_ANON
/Shared new-alloc tail: named-new jumps straight to EXP_CI_NEWSIZE (keeps
/its real lcount); the anonymous fall-through arrives with it cleared to
/0. op is already in RA_BS_RESULT from SK_ARG_Op above.
NOVA EXP_CI_NEWSIZE Op=Move El1=SK_NOVA_LCNT Exit=SK_TMP
NOVA EXP_CI_ABASE Op=Move El1=NOVA_SIZE Exit=RA_ALLOC_BASE
CLEAR EXP_CI_ATFORCE El1=RA_ALLOC_TERM_FORCE
/__LT_ALLOC_NOVA sentinel op: wire as Voca SR_GLR RA_LINK instead.
JEQ EXP_CI_ALTCK El1=RA_BS_RESULT El2=SK_ALLOC_NOVA_OP Exit=EXP_CI_ALTOP
NOVA EXP_CI_ASJ Op=Jump Exit=EXP_CI_ALLOC
NOLINK
NOVA EXP_CI_ALTOP Op=Move El1=Voca Exit=RA_BS_RESULT
NOVA Op=Move El1=SR_GLR Exit=SK_NOVA_E1
NOVA Op=Move El1=RA_LINK Exit=SK_NOVA_EXIT
NOVA Op=Jump Exit=EXP_CI_ALLOC
NOLINK
RVOCA EXP_CI_ALLOC Body=LOAD_ALLOC_SIZED
NOVA EXP_CI_SAVEADDR Op=Move El1=RA_ALLOC_RESULT Exit=SK_NOVA_ADDR
/Register the name cell (MA0) -> body; anonymous has nothing to register.
JEQ EXP_CI_REGCK El1=SK_CI_ANON El2=C_1 Exit=EXP_CI_WIRE
NOVA EXP_CI_REGN Op=Write El1=RA_MA0 El2=SK_NOVA_ADDR
/Hand the finished lux to WRITE_NOVA_SLOTS (synth.re): self-ref, op,
/e1/e2/exit, plus the autolink from RA_MC_PREV -- one shared writer for
/NOVA/NOVAE/DEF alike.
NOVA EXP_CI_WIRE Op=Move El1=SK_NOVA_ADDR Exit=RA_MC_LUX
NOVA EXP_CI_MCOP Op=Move El1=RA_BS_RESULT Exit=RA_MC_OP
NOVA EXP_CI_MCE1 Op=Move El1=SK_NOVA_E1 Exit=RA_MC_E1
NOVA EXP_CI_MCE2 Op=Move El1=SK_NOVA_E2 Exit=RA_MC_E2
NOVA EXP_CI_MCEX Op=Move El1=SK_NOVA_EXIT Exit=RA_MC_DEST
RVOCA EXP_CI_WIS Body=WRITE_NOVA_SLOTS
RREDI EXP_CI_RET
/LOAD_NOVA_READ_ARGS / _PENDING removed -- they were EXP_CMD_NOVA's
/own private El1=/El2=/Exit= reader, the last piece of its hand copy
/of the generic reader. EXP_CMD_NOVA now takes those from the
/pre-read (SK_ARG_El1/El2/Exit); no other caller existed.

/── EXP_CMD_NOVAE: self-hosted NOVAE (At= path) ────────────────────────────
/Mirrors loader.py _novae_wire. A NOVAE line synthesizes a *runtime builder*:
/a chain of ~6 NOVAs that, when Voca'd at dispatch time, write a target NOVA's
/slots via WRITE_NOVA_SLOTS. node0 IS the label (SYNTH_X); _op/_e1/_e2/_ex
/stage RA_MC_OP/E1/E2/DEST; _w Voca's WRITE_NOVA_SLOTS. Reuses the same ALLOC +
/WRITE_NOVA_SLOTS + AUTOLINK primitives NOVA/JEQ/... use -- no duplicate build
/logic. Args come from the generic pre-read (SK_ARG_At/Op/El1/El2/Exit/Op1/
/Op2/Op3/Reset). Op1/2/3 default to Move (per _novae_resolve_op). Covers the 33
/At= lines; Next=/arbitrary-extras/no-At (RCALL_AT) still fall through as
/unknown until added -- see loader.py _novae_wire for those two paths.
NEW RA_NE_TMP  /NOVAE: scratch for the Op1/2/3 default-Move choice
/── NOVAE_EMIT_NODE: alloc a fresh NOVA, write its 5 slots, autolink ──
/IN: RA_MC_OP/E1/E2/DEST already staged. OUT: RA_MC_LUX = the node's addr
/(and RA_MC_PREV advanced by AUTOLINK). The self-hosted _emit_nova.
NOLINK
NOVA NOVAE_EMIT_NODE Op=Move El1=NOVA_SIZE Exit=RA_ALLOC_COUNT
RVOCA NOVAE_EN_A Body=ALLOC
NOVA NOVAE_EN_L Op=Move El1=RA_ALLOC_RESULT Exit=RA_MC_LUX
RVOCA NOVAE_EN_W Body=WRITE_NOVA_SLOTS
RREDI NOVAE_EN_RET

/── NOVAE_STAGE_OP: RA_MC_OP <- (SK_ARG_OpN if given, else addr(Move)) ──
/IN: RA_NE_TMP = the SK_ARG_OpN value (0 = not given). Op-slot values are
/addresses, so Move on the arg (already resolved to addr) / Idem on Move.
NOLINK
JZ NOVAE_STAGE_OP El1=RA_NE_TMP Exit=NOVAE_SO_DEF
NOVA NOVAE_SO_USE Op=Move El1=RA_NE_TMP Exit=RA_MC_OP
RREDI NOVAE_SO_RET
NOLINK
NOVA NOVAE_SO_DEF Op=Idem El1=Move Exit=RA_MC_OP
RREDI NOVAE_SO_RET2

NOLINK
/Standalone builder: break the incoming autolink chain, zero the shared E2.
NOVA EXP_CMD_NOVAE Op=Move El1=C_0 Exit=RA_MC_PREV Cmd=NOVAE
NOVA ENV_E2Z Op=Move El1=C_0 Exit=RA_MC_E2
/node0: two shapes, chosen by At=. At=X reuses an existing lux (Move at->
/RA_MC_LUX). No At= means the target lux is allocated at the builder's OWN
/runtime, so node0 becomes three builder instructions that alloc a fresh
/NOVA into RA_MC_LUX (mirrors loader.py _novae_wire's __LT_ALLOC_NOVA path;
/the self-hosted allocator is ALLOC, same as NOVAE_EMIT_NODE uses). Both
/shapes register word(MA0)=first-node and fall into node1.
JZ ENV_ATCK El1=SK_ARG_At Exit=ENV_NOAT
/--- At= : node0 = Move at -> RA_MC_LUX ---
NOVA ENV_N0DST Op=Idem El1=RA_MC_LUX Exit=RA_MC_DEST
NOVA ENV_N0OP Op=Idem El1=Move Exit=RA_MC_OP
NOVA ENV_N0E1 Op=Move El1=SK_ARG_At Exit=RA_MC_E1
RVOCA ENV_N0 Body=NOVAE_EMIT_NODE
NOVA ENV_REG Op=Write El1=RA_MA0 El2=RA_MC_LUX
NOVA ENV_N0JMP Op=Jump Exit=ENV_N1DST
NOLINK
/--- no At= : node0 = alloc a fresh NOVA into RA_MC_LUX (3 builder nodes) ---
/a1: Move NOVA_SIZE -> RA_ALLOC_COUNT
NOVA ENV_NOAT Op=Idem El1=RA_ALLOC_COUNT Exit=RA_MC_DEST
NOVA ENV_NA1OP Op=Idem El1=Move Exit=RA_MC_OP
NOVA ENV_NA1E1 Op=Idem El1=NOVA_SIZE Exit=RA_MC_E1
RVOCA ENV_NA1 Body=NOVAE_EMIT_NODE
/register word(MA0)=a1 (the builder's entry) before RA_MC_LUX is overwritten
NOVA ENV_NA1REG Op=Write El1=RA_MA0 El2=RA_MC_LUX
/a2: Voca ALLOC -> RA_LINK  (allocates the fresh target, addr in RA_ALLOC_RESULT)
NOVA ENV_NA2DST Op=Idem El1=RA_LINK Exit=RA_MC_DEST
NOVA ENV_NA2OP Op=Idem El1=Voca Exit=RA_MC_OP
NOVA ENV_NA2E1 Op=Idem El1=ALLOC Exit=RA_MC_E1
RVOCA ENV_NA2 Body=NOVAE_EMIT_NODE
/a3: Move RA_ALLOC_RESULT -> RA_MC_LUX
NOVA ENV_NA3DST Op=Idem El1=RA_MC_LUX Exit=RA_MC_DEST
NOVA ENV_NA3OP Op=Idem El1=Move Exit=RA_MC_OP
NOVA ENV_NA3E1 Op=Idem El1=RA_ALLOC_RESULT Exit=RA_MC_E1
RVOCA ENV_NA3 Body=NOVAE_EMIT_NODE
/node1 (_op): Move op -> RA_MC_OP.
NOVA ENV_N1DST Op=Idem El1=RA_MC_OP Exit=RA_MC_DEST
NOVA ENV_N1OP Op=Idem El1=Move Exit=RA_MC_OP
NOVA ENV_N1E1 Op=Move El1=SK_ARG_Op Exit=RA_MC_E1
RVOCA ENV_N1 Body=NOVAE_EMIT_NODE
/node2 (_e1): (Op1|Move) El1 -> RA_MC_E1.
NOVA ENV_N2DST Op=Idem El1=RA_MC_E1 Exit=RA_MC_DEST
NOVA ENV_N2OPT Op=Move El1=SK_ARG_Op1 Exit=RA_NE_TMP
RVOCA ENV_N2OP Body=NOVAE_STAGE_OP
NOVA ENV_N2E1 Op=Move El1=SK_ARG_El1 Exit=RA_MC_E1
RVOCA ENV_N2 Body=NOVAE_EMIT_NODE
/node3 (_e2): (Op2|Move) El2 -> RA_MC_E2.
NOVA ENV_N3DST Op=Idem El1=RA_MC_E2 Exit=RA_MC_DEST
NOVA ENV_N3OPT Op=Move El1=SK_ARG_Op2 Exit=RA_NE_TMP
RVOCA ENV_N3OP Body=NOVAE_STAGE_OP
NOVA ENV_N3E1 Op=Move El1=SK_ARG_El2 Exit=RA_MC_E1
RVOCA ENV_N3 Body=NOVAE_EMIT_NODE
/node4 (_ex): (Op3|Move) Exit -> RA_MC_DEST. Its own DEST target is the
/RA_MC_DEST register itself, so Idem(RA_MC_DEST)->RA_MC_DEST first.
NOVA ENV_N4DST Op=Idem El1=RA_MC_DEST Exit=RA_MC_DEST
NOVA ENV_N4OPT Op=Move El1=SK_ARG_Op3 Exit=RA_NE_TMP
RVOCA ENV_N4OP Body=NOVAE_STAGE_OP
NOVA ENV_N4E1 Op=Move El1=SK_ARG_Exit Exit=RA_MC_E1
RVOCA ENV_N4 Body=NOVAE_EMIT_NODE
/node5 (_w): Voca WRITE_NOVA_SLOTS[_RESET] -> RA_LINK.
NOVA ENV_N5DST Op=Idem El1=RA_LINK Exit=RA_MC_DEST
NOVA ENV_N5OP Op=Idem El1=Voca Exit=RA_MC_OP
JZ ENV_RSTCK El1=SK_ARG_Reset Exit=ENV_N5_NORST
NOVA ENV_N5E1R Op=Idem El1=WRITE_NOVA_SLOTS_RESET Exit=RA_MC_E1
NOVA ENV_N5RJ Op=Jump Exit=ENV_N5EMIT
NOLINK
NOVA ENV_N5_NORST Op=Idem El1=WRITE_NOVA_SLOTS Exit=RA_MC_E1
RVOCA ENV_N5EMIT Body=NOVAE_EMIT_NODE
/Builder complete; leave a clean autolink boundary for the next line.
NOVA ENV_DONE Op=Move El1=C_0 Exit=RA_MC_PREV
RREDI EXP_CMD_NOVAE_RET
