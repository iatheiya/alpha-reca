/============================================================
//aria/htable.re — Generic Hash Table

Probe-based hash table over Reca Lux.
All subroutines use native Voca/Redi (leaf — Redi via RA_LINK).

Slot format: word = (hash32 << 32) | lux_id.  Empty = 0.
Probe: linear. Size must be power of 2. Mask = size-1.

HT_LOOKUP:  IN: RA_HT_HASH, RA_HT_BASE, RA_HT_MASK, RA_HT_SIZE
OUT: RA_HT_RESULT = found lux_id or 0
HT_INSERT:  IN: RA_HT_HASH, RA_HT_LID, RA_HT_BASE, RA_HT_MASK, RA_HT_SIZE
HT_CLEAR:   IN: RA_HT_BASE, RA_HT_SIZE

DEPENDENCY: aspects.re  core/constants.re (C_0, C_1, C_32)
aria/math.re (MASK_LOW32)//

NEW RA_HT_BASE
NEW RA_HT_HASH
NEW RA_HT_SIZE
NEW RA_HT_MASK
NEW RA_HT_RESULT
NEW RA_HT_LID
NEW RA_HT_SLOT
NEW RA_HT_WORD
NEW RA_HT_STORED_H
NEW RA_HT_PROBE

── HT_LOOKUP ─────────────────────────────────────────────────
ITO HT_LOOKUP     And       El1=RA_HT_HASH  El2=RA_HT_MASK   Exit=RA_HT_SLOT
CLEAR HT_LK_PC RA_HT_PROBE
ITO HT_LK_ADDR    Add       El1=RA_HT_BASE  El2=RA_HT_SLOT   Exit=RA_HT_WORD
ITO HT_LK_LOAD    Read   El1=RA_HT_WORD  Exit=RA_HT_WORD
JZ HT_LK_EZ RA_HT_WORD HT_LK_NF
ITO HT_LK_CMP     Right       El1=RA_HT_WORD  El2=C_32         Exit=RA_HT_STORED_H
ITO HT_LK_HM      Equal El1=RA_HT_STORED_H El2=RA_HT_HASH Exit=RA_HT_STORED_H
ITO HT_LK_HM_INV  Sub       El1=C_1         El2=RA_HT_STORED_H Exit=RA_HT_STORED_H
ITO HT_LK_HMJ     JumpIf    El1=RA_HT_STORED_H                Exit=HT_LK_NEXT
ITO HT_LK_FOUND   And       El1=RA_HT_WORD  El2=MASK_LOW32   Exit=RA_HT_RESULT
RREDI HT_LK_FRET
ITO HT_LK_NEXT    Add       El1=RA_HT_SLOT  El2=C_1          Exit=RA_HT_SLOT
ITO HT_LK_WRAP    And       El1=RA_HT_SLOT  El2=RA_HT_MASK   Exit=RA_HT_SLOT
ITO HT_LK_PI      Add       El1=RA_HT_PROBE El2=C_1          Exit=RA_HT_PROBE
JEQ HT_LK_FULL RA_HT_PROBE RA_HT_SIZE HT_LK_NF
ITO HT_LK_LOOP    Jump      Exit=HT_LK_ADDR
CLEAR HT_LK_NF RA_HT_RESULT
RREDI HT_LK_NFRET

── HT_INSERT (upsert) ────────────────────────────────────────
//Inserts or updates: if hash already present → overwrites value.
Does NOT mutate RA_HT_LID (writes masked lid into RA_HT_SLOT as scratch).
Leaf (Redi via RA_LINK).//
ITO HT_INSERT     And       El1=RA_HT_HASH  El2=RA_HT_MASK   Exit=RA_HT_SLOT
CLEAR HT_IN_PC RA_HT_PROBE
ITO HT_IN_ADDR    Add       El1=RA_HT_BASE  El2=RA_HT_SLOT   Exit=RA_HT_WORD
ITO HT_IN_LOAD    Read   El1=RA_HT_WORD  Exit=RA_HT_STORED_H
/Empty slot → write new entry
JZ HT_IN_EZ RA_HT_STORED_H HT_IN_WRITE
//Not empty: reload raw word (STORED_H was overwritten by Equal)
Re-read to extract hash for upsert check//
ITO HT_IN_RELOAD  Read   El1=RA_HT_WORD  Exit=RA_HT_STORED_H
ITO HT_IN_EXTH    Right       El1=RA_HT_STORED_H El2=C_32      Exit=RA_HT_STORED_H
JEQ HT_IN_HCMP RA_HT_STORED_H RA_HT_HASH HT_IN_WRITE
/Probe forward
ITO HT_IN_NEXT    Add       El1=RA_HT_SLOT  El2=C_1          Exit=RA_HT_SLOT
ITO HT_IN_WRAP    And       El1=RA_HT_SLOT  El2=RA_HT_MASK   Exit=RA_HT_SLOT
ITO HT_IN_PI      Add       El1=RA_HT_PROBE El2=C_1          Exit=RA_HT_PROBE
JEQ HT_IN_FULL RA_HT_PROBE RA_HT_SIZE HT_IN_FULL_RET
ITO HT_IN_LOOP    Jump      Exit=HT_IN_ADDR
//Write: pack (hash << 32) | (lid & MASK_LOW32) into slot
RA_HT_WORD still holds the slot address (Exit=HT_IN_ADDR recomputes it correctly)//
ITO HT_IN_WRITE   Left       El1=RA_HT_HASH  El2=C_32         Exit=RA_HT_STORED_H
ITO HT_IN_LIDMSK  And       El1=RA_HT_LID   El2=MASK_LOW32   Exit=RA_HT_SLOT
ITO HT_IN_WVAL    Add       El1=RA_HT_STORED_H El2=RA_HT_SLOT Exit=RA_HT_STORED_H
ITO HT_IN_STORE   Write  El1=RA_HT_WORD  El2=RA_HT_STORED_H
RREDI HT_IN_RET
── HT_OVERFLOW_VEC ────────────────────────────────────────────
//word = handler Lux ID, jumped to when HT_INSERT finds no free slot.
Default: HT_OVF_DEFAULT = End (halt). Override via:
SETREF HT_OVERFLOW_VEC my_handler
This makes overflow handling reversible/customisable per Reca convention.//
NEW HT_OVERFLOW_VEC
NOLINK
ITO HT_IN_FULL_RET JumpReg El1=HT_OVERFLOW_VEC
NOLINK
ITO HT_OVF_DEFAULT End
SETREF HT_OVERFLOW_VEC HT_OVF_DEFAULT

── HT_CLEAR ──────────────────────────────────────────────────
/IN: RA_HT_BASE, RA_HT_SIZE. Leaf.
NOLINK
CLEAR HT_CLEAR RA_HT_SLOT
JEQ HT_CL_LOOP RA_HT_SLOT RA_HT_SIZE HT_CL_DONE
ITO HT_CL_ADDR   Add       El1=RA_HT_BASE  El2=RA_HT_SLOT   Exit=RA_HT_WORD
ITO HT_CL_ZERO   Write  El1=RA_HT_WORD  El2=C_0
ITO HT_CL_INC    Add       El1=RA_HT_SLOT  El2=C_1          Exit=RA_HT_SLOT
ITO HT_CL_LB     Jump      Exit=HT_CL_LOOP
RREDI HT_CL_DONE