//flux.re — Flux allocation and structure traversal

── What is a flux ────────────────────────────────────────────────────────
Flux = Data Lux of arbitrary structure, living in the flux zone
(FLUX_BOTTOM <= addr < STACK_BOTTOM). Detected by addr >= FLUX_BOTTOM
(one compare, only on explicit jump — hot path never checks this).

Structure:
  [word = type_lux_addr, slot1, slot2, ..., slotN]

word (slot 0) points to a type lux (Data Lux) that describes the layout.
Type lux = [any_word, code1, code2, ..., 0]
Codes (FLUX_SLOT_*): 1=Word 2=Op 3=El1 4=El2 5=Exit 6=Next 7=Rel

Next (code 6) may appear multiple times → multiple continuation points (fan-out).
This is the foundation for parallelism: Aria scheduler reads all Next addrs
and launches them via Exire(clone). The core provides only the structure.

Allocation: ALLOC_FLUX_ZONE (alloc.re). bump allocator in flux zone.
No predefined types — type lux is created dynamically by caller.

DEPENDENCY: aspects.re  constants.re  regs.re  alloc.re//

── Registers ──────────────────────────────────────────────────────────────────
NEW RA_FX_SRC      /flux address (input to read functions)
NEW RA_FX_TYPE     /type lux address (read from RA_FX_SRC word)
NEW RA_FX_POS      /current position while traversing flux
NEW RA_FX_CODE     /current slot code from type lux
NEW RA_FX_VAL      /current slot value from flux
NEW RA_FX_SLOT     /slot index within flux (0-based, after word)
NEW RA_FX_TYPE_POS /current position within type lux (scanning codes)
NEW RA_FX_RESULT   /output: found slot value
NEW RA_FX_FIND     /input: slot code to find (for FLUX_FIND_SLOT)
NEW RA_FX_COUNT    /count of Next slots found (for FLUX_COUNT_NEXT)

── ALLOC_FLUX: allocate flux of RA_ALLOC_COUNT slots ────────────────
//IN:  RA_ALLOC_COUNT = total slots needed (including word slot)
        RA_FX_TYPE = type lux address (will be written into word slot)
OUT: RA_ALLOC_RESULT = flux address
Caller fills remaining slots after return. Non-leaf.//
NOLINK
    RVOCA ALLOC_FLUX   ALLOC_FLUX_ZONE
    /Write type lux addr into word slot (slot 0) of new flux
    ITO AFX_WRITE    Write El1=RA_ALLOC_RESULT El2=RA_FX_TYPE

── FLUX_READ_TYPE: read type lux addr from flux word slot (kept for scheduler.re) ──
//IN:  RA_FX_SRC = flux address
OUT: RA_FX_TYPE = type lux address. Leaf.//
NOLINK
ITO FLUX_READ_TYPE  Read  El1=RA_FX_SRC       Exit=RA_FX_TYPE
RREDI HRT_RET

── FLUX_INIT_SCAN: read type lux and init scan position ──────────────────
//IN:  RA_FX_SRC = flux address
OUT: RA_FX_TYPE = type lux addr, RA_FX_TYPE_POS = first code pos (type+1). Leaf.//
NOLINK
ITO FLUX_INIT_SCAN  Read  El1=RA_FX_SRC      Exit=RA_FX_TYPE
ITO FIS_INIT_TP     Add   El1=RA_FX_TYPE El2=C_1 Exit=RA_FX_TYPE_POS
RREDI FIS_RET

── FLUX_FIND_SLOT: find first slot with given code in flux ───────────────
//IN:  RA_FX_SRC = flux address
        RA_FX_FIND = slot code to find (FLUX_SLOT_*)
OUT: RA_FX_RESULT = value at that slot (0 if not found)
     RA_FLAG = 1 if found, 0 if not found
Reads type lux to know slot positions. Non-leaf.//
NOLINK
    RVOCA FLUX_FIND_SLOT FLUX_INIT_SCAN
    ITO HFS_INIT_HP  Add  El1=RA_FX_SRC   El2=C_1  Exit=RA_FX_POS
    CLEAR HFS_INIT_FG RA_FLAG                   /not found yet
    NOLINK
    /Loop: read next code from type lux
    ITO HFS_LOOP     Read  El1=RA_FX_TYPE_POS  Exit=RA_FX_CODE
    JZ HFS_CODEZ RA_FX_CODE HFS_DONE          /code==0 → end of type lux
    /Read corresponding value from flux
    ITO HFS_RDVAL    Read  El1=RA_FX_POS        Exit=RA_FX_VAL
    /Check if this is the slot we're looking for
    JEQ HFS_CK RA_FX_CODE RA_FX_FIND HFS_FOUND
    /Advance both positions
    ITO HFS_ADVT     Add   El1=RA_FX_TYPE_POS El2=C_1  Exit=RA_FX_TYPE_POS
    ITO HFS_ADVH     Add   El1=RA_FX_POS      El2=C_1  Exit=RA_FX_POS
    ITO HFS_LB       Jump  Exit=HFS_LOOP
    NOLINK
    /Found: set result and flag
    ITO HFS_FOUND    Move  El1=RA_FX_VAL  Exit=RA_FX_RESULT
    ITO HFS_SETFG    Move  El1=C_1        Exit=RA_FLAG
    NOLINK
    /Done (found or end of type lux)
    RREDI HFS_DONE
── FLUX_COUNT_NEXT: count Next slots in flux ────────────────────────────
//IN:  RA_FX_SRC = flux address
OUT: RA_FX_COUNT = number of Next slots (FLUX_SLOT_NEXT codes in type lux)
Used by Aria scheduler to know how many continuation points exist. Leaf.//
NOLINK
    RVOCA FLUX_COUNT_NEXT FLUX_INIT_SCAN
    CLEAR HCN_INIT_CNT RA_FX_COUNT
    NOLINK
    ITO HCN_LOOP     Read  El1=RA_FX_TYPE_POS  Exit=RA_FX_CODE
    JZ HCN_CODEZ RA_FX_CODE HCN_DONE
    JEQ HCN_CK RA_FX_CODE FLUX_SLOT_NEXT HCN_INC
    ITO HCN_ADVT     Add   El1=RA_FX_TYPE_POS El2=C_1  Exit=RA_FX_TYPE_POS
    ITO HCN_LB       Jump  Exit=HCN_LOOP
    NOLINK
    ITO HCN_INC      Add   El1=RA_FX_COUNT El2=C_1  Exit=RA_FX_COUNT
    ITO HCN_INCADV   Add   El1=RA_FX_TYPE_POS El2=C_1 Exit=RA_FX_TYPE_POS
    ITO HCN_INCLB    Jump  Exit=HCN_LOOP
    NOLINK
    RREDI HCN_DONE