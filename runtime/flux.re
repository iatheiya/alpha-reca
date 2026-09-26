//
flux.re — Flux allocation and structure traversal

── What is a flux ────────────────────────────────────────────────────────
Flux = Data Lux of arbitrary structure, living in the flux zone
(FLUX_BOTTOM <= addr < STACK_BOTTOM). Detected by addr >= FLUX_BOTTOM
(one compare, only on explicit jump — line path never checks this).

Structure (one lux, no separate type lux):
  [descriptor, val1, val2, ..., 0]

descriptor (slot 0) is a sequence of 4-bit digits, unpacked from the
least-significant nibble upward, 0-terminated. A plain digit (code
1-7, FLUX_SLOT_*) consumes exactly one value in order. FLUX_GROUP
(14) / FLUX_PIVOT (13) mark a group — "FLUX_GROUP code(s)... FLUX_PIVOT
count FLUX_GROUP", always paired. One code before the pivot means
"one-to-many" (that code gets `count` separate values); several codes
before the pivot means "many-to-one" (those codes share the same
value(s)). See constants.re's own Flux slot type codes section and
interpreter.py's _exec_flux for the full reference decode.

Allocation: ALLOC_FLUX_ZONE (alloc.re). bump allocator in flux zone.

DEPENDENCY: aspects.re  constants.re  registers.re  alloc.re
//

── Registers ──────────────────────────────────────────────────────────────────
NEW RA_FX_SRC  /flux address (input to read functions)
NEW RA_FX_DESC  /descriptor being unpacked (destructively shifted per nibble)
NEW RA_FX_NIB  /current nibble extracted from RA_FX_DESC
NEW RA_FX_VAL_POS  /current value-read cursor into the flux
NEW RA_FX_RESULT  /output: found value
NEW RA_FX_FIND  /input: code to find (for FLUX_FIND_SLOT)
NEW RA_FX_COUNT  /count of Next occurrences (for FLUX_COUNT_NEXT)
NEW RA_FX_GLEFTN  /scratch: how many codes seen before FLUX_PIVOT in current group
NEW RA_FX_GFOUND  /scratch: whether the sought code was among a group's left codes
NEW RA_FX_GCOUNT  /scratch: current group's count digit (after FLUX_PIVOT)
NEW RA_FX_ITER_CODE  /input: code the FLUX_NEXT_ITER_* pair walks
NEW RA_FX_GREM  /iterator state: values left to emit from an in-progress one-to-many group

── ALLOC_FLUX: allocate flux of RA_ALLOC_COUNT slots ────────────────
//
IN:  RA_ALLOC_COUNT = total slots needed (including descriptor slot)
        RA_FX_DESC = descriptor (will be written into slot 0)
OUT: RA_ALLOC_RESULT = flux address
Caller fills remaining slots after return. Non-leaf.
//
NOLINK
    RVOCA ALLOC_FLUX Body=ALLOC_FLUX_ZONE
    /Write descriptor into slot 0 of new flux/
    NOVA AFX_WRITE Op=Write El1=RA_ALLOC_RESULT El2=RA_FX_DESC
    /Explicit return -- ALLOC_FLUX is currently dead code (never called
    /via RVOCA anywhere in the project), so this fallthrough is currently
    /harmless, but relies on address adjacency exactly like this
    /session's other Pattern Z fixes; fixing now so it isn't a landmine
    /the moment something starts calling it.
    RREDI AFX_RET

── FLUX_INIT_SCAN: read descriptor and init scan position ────────────────
//
IN:  RA_FX_SRC = flux address
OUT: RA_FX_DESC = descriptor, RA_FX_VAL_POS = first value pos (src+1). Leaf.
//
NOLINK
NOVA FLUX_INIT_SCAN Op=Read El1=RA_FX_SRC Exit=RA_FX_DESC
NOVA FIS_INIT_VP Op=Add El1=RA_FX_SRC El2=C_1 Exit=RA_FX_VAL_POS
/FISC_RET, not FIS_RET: FLUX_NEXT_ITER_STEP below reuses the FIS_ prefix and
/has its own `RREDI FIS_RET`. As the first definition this one used to win
/the FIS_RET symbol, so STEP's own Exit=FIS_RET jumps silently landed on this
/RREDI instead of STEP's. Both are plain RREDIs so behaviour matched, but it
/was a real duplicate label; a distinct name here keeps each return its own.
RREDI FISC_RET

── FLUX_NEXT_NIBBLE: pop the next 4-bit digit off RA_FX_DESC ──────────────
//
IN/OUT: RA_FX_DESC (consumed, shifted right by 4)
OUT: RA_FX_NIB = extracted nibble (0 means descriptor exhausted). Leaf.
//
NOLINK
NOVA FLUX_NEXT_NIBBLE Op=And El1=RA_FX_DESC El2=C_15 Exit=RA_FX_NIB
NOVA FNN_SHIFT Op=Right El1=RA_FX_DESC El2=C_4 Exit=RA_FX_DESC
RREDI FNN_RET

── FLUX_FIND_SLOT: find first value for a given code in flux ─────────────
//
IN:  RA_FX_SRC = flux address
        RA_FX_FIND = code to find (FLUX_SLOT_*)
OUT: RA_FX_RESULT = value for that code (0 if not found)
     RA_FLAG = 1 if found, 0 if not found
Walks the descriptor directly (no separate type lux to read). Non-leaf.
//
NOLINK
    RVOCA FLUX_FIND_SLOT Body=FLUX_INIT_SCAN
    CLEAR HFS_INIT_FG El1=RA_FLAG /not found yet
    NOVA HFS_LOOP Op=Jump Exit=HFS_STEP
    NOLINK
    RVOCA HFS_STEP Body=FLUX_NEXT_NIBBLE
    JZ HFS_NIBZ El1=RA_FX_NIB Exit=HFS_DONE /descriptor exhausted -- not found
    JEQ HFS_GCK El1=RA_FX_NIB El2=FLUX_GROUP Exit=HFS_GROUP
    /plain code/
    JEQ HFS_PCK El1=RA_FX_NIB El2=RA_FX_FIND Exit=HFS_PFOUND
    /not the one we want -- skip its value, keep scanning/
    NOVA HFS_PSKIP Op=Add El1=RA_FX_VAL_POS El2=C_1 Exit=RA_FX_VAL_POS
    NOVA HFS_PLB Op=Jump Exit=HFS_LOOP
    NOLINK
    NOVA HFS_PFOUND Op=Read El1=RA_FX_VAL_POS Exit=RA_FX_RESULT
    NOVA HFS_PSETFG Op=Move El1=C_1 Exit=RA_FLAG
    NOVA HFS_PDJ Op=Jump Exit=HFS_DONE
    NOLINK
    /group: collect left codes until FLUX_PIVOT, remembering if RA_FX_FIND was among them/
    NOLINK
    CLEAR HFS_GROUP El1=RA_FX_GFOUND
    NOVA HFS_GLOOP Op=Jump Exit=HFS_GSTEP
    NOLINK
    RVOCA HFS_GSTEP Body=FLUX_NEXT_NIBBLE
    JEQ HFS_GPIVCK El1=RA_FX_NIB El2=FLUX_PIVOT Exit=HFS_GPIVOT
    JEQ HFS_GMCK El1=RA_FX_NIB El2=RA_FX_FIND Exit=HFS_GMARK
    NOVA HFS_GLB Op=Jump Exit=HFS_GLOOP
    NOLINK
    NOVA HFS_GMARK Op=Move El1=C_1 Exit=RA_FX_GFOUND
    NOVA HFS_GMJ Op=Jump Exit=HFS_GLOOP
    NOLINK
    RVOCA HFS_GPIVOT Body=FLUX_NEXT_NIBBLE /count digit/
    NOVA HFS_GCNT Op=Move El1=RA_FX_NIB Exit=RA_FX_GCOUNT
    RVOCA HFS_GCLOSE Body=FLUX_NEXT_NIBBLE /consume closing FLUX_GROUP/
    JZ HFS_GFCK El1=RA_FX_GFOUND Exit=HFS_GSKIP
    /found in this group -- first value is correct either way (one-to-many:
    /its first of N; many-to-one: the one shared value)/
    NOVA HFS_GRD Op=Read El1=RA_FX_VAL_POS Exit=RA_FX_RESULT
    NOVA HFS_GSETFG Op=Move El1=C_1 Exit=RA_FLAG
    NOVA HFS_GFDJ Op=Jump Exit=HFS_DONE
    NOLINK
    /not in this group -- skip its RA_FX_GCOUNT values, keep scanning/
    NOVA HFS_GSKIP Op=Add El1=RA_FX_VAL_POS El2=RA_FX_GCOUNT Exit=RA_FX_VAL_POS
    NOVA HFS_GSKJ Op=Jump Exit=HFS_LOOP
    NOLINK
    RREDI HFS_DONE

── FLUX_COUNT_NEXT: count Next occurrences in flux ────────────────────────
//
IN:  RA_FX_SRC = flux address
OUT: RA_FX_COUNT = number of FLUX_SLOT_NEXT values (plain + inside groups)
Used by Aria scheduler to know how many continuation points exist. Non-leaf.
//
NOLINK
    RVOCA FLUX_COUNT_NEXT Body=FLUX_INIT_SCAN
    CLEAR HCN_INIT_CNT El1=RA_FX_COUNT
    NOVA HCN_LOOP Op=Jump Exit=HCN_STEP
    NOLINK
    RVOCA HCN_STEP Body=FLUX_NEXT_NIBBLE
    JZ HCN_NIBZ El1=RA_FX_NIB Exit=HCN_DONE
    JEQ HCN_GCK El1=RA_FX_NIB El2=FLUX_GROUP Exit=HCN_GROUP
    JEQ HCN_PCK El1=RA_FX_NIB El2=FLUX_SLOT_NEXT Exit=HCN_PINC
    NOVA HCN_PSKIP Op=Add El1=RA_FX_VAL_POS El2=C_1 Exit=RA_FX_VAL_POS
    NOVA HCN_PLB Op=Jump Exit=HCN_LOOP
    NOLINK
    NOVA HCN_PINC Op=Add El1=RA_FX_COUNT El2=C_1 Exit=RA_FX_COUNT
    NOVA HCN_PINCA Op=Add El1=RA_FX_VAL_POS El2=C_1 Exit=RA_FX_VAL_POS
    NOVA HCN_PILB Op=Jump Exit=HCN_LOOP
    NOLINK
    CLEAR HCN_GROUP El1=RA_FX_GLEFTN
    CLEAR HCN_GINIT El1=RA_FX_GFOUND
    NOVA HCN_GLOOP Op=Jump Exit=HCN_GSTEP
    NOLINK
    RVOCA HCN_GSTEP Body=FLUX_NEXT_NIBBLE
    JEQ HCN_GPIVCK El1=RA_FX_NIB El2=FLUX_PIVOT Exit=HCN_GPIVOT
    NOVA HCN_GLN Op=Add El1=RA_FX_GLEFTN El2=C_1 Exit=RA_FX_GLEFTN
    JEQ HCN_GMCK El1=RA_FX_NIB El2=FLUX_SLOT_NEXT Exit=HCN_GMARK
    NOVA HCN_GLB Op=Jump Exit=HCN_GLOOP
    NOLINK
    NOVA HCN_GMARK Op=Move El1=C_1 Exit=RA_FX_GFOUND
    NOVA HCN_GMJ Op=Jump Exit=HCN_GLOOP
    NOLINK
    RVOCA HCN_GPIVOT Body=FLUX_NEXT_NIBBLE /count digit/
    NOVA HCN_GCNT Op=Move El1=RA_FX_NIB Exit=RA_FX_GCOUNT
    RVOCA HCN_GCLOSE Body=FLUX_NEXT_NIBBLE /consume closing FLUX_GROUP/
    JZ HCN_GFCK El1=RA_FX_GFOUND Exit=HCN_GSKIP
    JEQ HCN_G1CK El1=RA_FX_GLEFTN El2=C_1 Exit=HCN_G1M
    /many-to-one: Next was one of several codes sharing one value -> +1/
    NOVA HCN_GMTOI Op=Add El1=RA_FX_COUNT El2=C_1 Exit=RA_FX_COUNT
    NOVA HCN_GMTOJ Op=Jump Exit=HCN_GADV
    NOLINK
    /one-to-many and the single code IS Next -> +count/
    NOVA HCN_G1M Op=Add El1=RA_FX_COUNT El2=RA_FX_GCOUNT Exit=RA_FX_COUNT
    NOVA HCN_G1MJ Op=Jump Exit=HCN_GADV
    NOLINK
    NOVA HCN_GSKIP Op=Move El1=RA_FX_COUNT Exit=RA_FX_COUNT /no-op, keep shape/
    NOVA HCN_GADV Op=Add El1=RA_FX_VAL_POS El2=RA_FX_GCOUNT Exit=RA_FX_VAL_POS
    NOVA HCN_GADVJ Op=Jump Exit=HCN_LOOP
    NOLINK
    RREDI HCN_DONE

── FLUX_NEXT_ITER_INIT / FLUX_NEXT_ITER_STEP: walk every value for one code ──
//
Replaces manually re-scanning the descriptor once per Next slot
(what SCHED_FORK used to do against the old type lux). INIT sets up
scan state once; STEP returns one value per call (RA_FLAG=1) until
the descriptor is exhausted (RA_FLAG=0). Handles a one-to-many group
by remembering how many of its values are still owed (RA_FX_GREM)
across calls.

IN (INIT):  RA_FX_SRC, RA_FX_ITER_CODE = code to walk (e.g. FLUX_SLOT_NEXT)
OUT (STEP): RA_FLAG, RA_FX_RESULT (when RA_FLAG=1)
//
NOLINK
    RVOCA FLUX_NEXT_ITER_INIT Body=FLUX_INIT_SCAN
    CLEAR FNI_GREM El1=RA_FX_GREM
    RREDI FNI_RET

NOLINK
    NOVA FLUX_NEXT_ITER_STEP Op=Jump Exit=FIS_CHECK_REM
    NOLINK
    /still owed values from an in-progress one-to-many group?/
    NOVA FIS_CHECK_REM Op=Jump Exit=FIS_REMCK
    NOLINK
    JZ FIS_REMCK El1=RA_FX_GREM Exit=FIS_SCAN
    NOVA FIS_REMRD Op=Read El1=RA_FX_VAL_POS Exit=RA_FX_RESULT
    NOVA FIS_REMADV Op=Add El1=RA_FX_VAL_POS El2=C_1 Exit=RA_FX_VAL_POS
    NOVA FIS_REMDEC Op=Sub El1=RA_FX_GREM El2=C_1 Exit=RA_FX_GREM
    NOVA FIS_REMFG Op=Move El1=C_1 Exit=RA_FLAG
    NOVA FIS_REMDJ Op=Jump Exit=FIS_RET
    NOLINK
    RVOCA FIS_SCAN Body=FLUX_NEXT_NIBBLE
    JZ FIS_NIBZ El1=RA_FX_NIB Exit=FIS_EXHAUST
    JEQ FIS_GCK El1=RA_FX_NIB El2=FLUX_GROUP Exit=FIS_GROUP
    JEQ FIS_PCK El1=RA_FX_NIB El2=RA_FX_ITER_CODE Exit=FIS_PFOUND
    NOVA FIS_PSKIP Op=Add El1=RA_FX_VAL_POS El2=C_1 Exit=RA_FX_VAL_POS
    NOVA FIS_PLB Op=Jump Exit=FIS_SCAN
    NOLINK
    NOVA FIS_PFOUND Op=Read El1=RA_FX_VAL_POS Exit=RA_FX_RESULT
    NOVA FIS_PADV Op=Add El1=RA_FX_VAL_POS El2=C_1 Exit=RA_FX_VAL_POS
    NOVA FIS_PSETFG Op=Move El1=C_1 Exit=RA_FLAG
    NOVA FIS_PDJ Op=Jump Exit=FIS_RET
    NOLINK
    CLEAR FIS_GROUP El1=RA_FX_GLEFTN
    CLEAR FIS_GINIT El1=RA_FX_GFOUND
    NOVA FIS_GLOOP Op=Jump Exit=FIS_GSTEP
    NOLINK
    RVOCA FIS_GSTEP Body=FLUX_NEXT_NIBBLE
    JEQ FIS_GPIVCK El1=RA_FX_NIB El2=FLUX_PIVOT Exit=FIS_GPIVOT
    NOVA FIS_GLN Op=Add El1=RA_FX_GLEFTN El2=C_1 Exit=RA_FX_GLEFTN
    JEQ FIS_GMCK El1=RA_FX_NIB El2=RA_FX_ITER_CODE Exit=FIS_GMARK
    NOVA FIS_GLB Op=Jump Exit=FIS_GLOOP
    NOLINK
    NOVA FIS_GMARK Op=Move El1=C_1 Exit=RA_FX_GFOUND
    NOVA FIS_GMJ Op=Jump Exit=FIS_GLOOP
    NOLINK
    RVOCA FIS_GPIVOT Body=FLUX_NEXT_NIBBLE /count digit/
    NOVA FIS_GCNT Op=Move El1=RA_FX_NIB Exit=RA_FX_GCOUNT
    RVOCA FIS_GCLOSE Body=FLUX_NEXT_NIBBLE /consume closing FLUX_GROUP/
    JZ FIS_GFCK El1=RA_FX_GFOUND Exit=FIS_GSKIP
    JEQ FIS_G1CK El1=RA_FX_GLEFTN El2=C_1 Exit=FIS_G1M
    /many-to-one: exactly one hit, one shared value/
    NOVA FIS_GMRD Op=Read El1=RA_FX_VAL_POS Exit=RA_FX_RESULT
    NOVA FIS_GMADV Op=Add El1=RA_FX_VAL_POS El2=RA_FX_GCOUNT Exit=RA_FX_VAL_POS
    NOVA FIS_GMFG Op=Move El1=C_1 Exit=RA_FLAG
    NOVA FIS_GMDJ Op=Jump Exit=FIS_RET
    NOLINK
    /one-to-many: emit the first value now, owe RA_FX_GCOUNT-1 more/
    NOVA FIS_G1M Op=Read El1=RA_FX_VAL_POS Exit=RA_FX_RESULT
    NOVA FIS_G1MADV Op=Add El1=RA_FX_VAL_POS El2=C_1 Exit=RA_FX_VAL_POS
    NOVA FIS_G1MREM Op=Sub El1=RA_FX_GCOUNT El2=C_1 Exit=RA_FX_GREM
    NOVA FIS_G1MFG Op=Move El1=C_1 Exit=RA_FLAG
    NOVA FIS_G1MDJ Op=Jump Exit=FIS_RET
    NOLINK
    NOVA FIS_GSKIP Op=Add El1=RA_FX_VAL_POS El2=RA_FX_GCOUNT Exit=RA_FX_VAL_POS
    NOVA FIS_GSKJ Op=Jump Exit=FIS_SCAN
    NOLINK
    NOVA FIS_EXHAUST Op=Move El1=C_0 Exit=RA_FLAG
    RREDI FIS_RET
