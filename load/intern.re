/intern.re — Symbol table and name interning for the Reca loader
/
/Contains the symbol hash table (one large BLOCK in aether),
/and the three intern operations built on top of it:
/
/  BS_LOOKUP       — find token by hash → RA_BS_RESULT (0 if absent)
/  BS_INTERN       — lookup or lazy-alloc 1-lux entry (no name stored)
/  BS_INTERN_NAMED — lookup or lazy-alloc 2-lux entry, packed name at lux[1]
/
/Also owns:
/  RA_BS_RESULT — result register shared by all three operations
/  RA_BS_EL0/EL1 — element addr scratch shared with saku.re dispatch
/  RA_BS_ELC    — element count scratch (also used by SWITCH in macros.re)
/  BS_LAST_ITO  — autolink state: addr of last ITO lux in current chain
/
/All callers set RA_LOAD_HASH before calling BS_LOOKUP/BS_INTERN.
/BS_INTERN_NAMED also expects BS_TOKBUF_BASE+RA_LOAD_TLEN to be valid
/(calls BS_PACK_TOKBUF from lexer.re).
/
/DEPENDS ON: aspects.re constants.re alloc.re htable.re lexer.re math.re

/── Symbol hash table ─────────────────────────────────────────
/Probe-based, linear. Slot = (hash32 << 32) | lux_addr.
/Size must be power of 2. 4096 slots (262144 luces total).
NEWSET BS_HT_SIZE  262144
NEWSET BS_HT_MASK  262143
BLOCK  BS_HTAB_000 262144  /symbol table storage in aether
SETREF BS_HTAB_000 BS_HTAB_000  /self-ref: BS_INIT reads word to get base addr

NEW BS_HT_BASE   /base lux addr of BS_HTAB (written by BS_INIT in lexer.re)

/── Shared result and element registers ───────────────────────
NEW RA_BS_RESULT  /result of BS_LOOKUP/BS_INTERN: lux addr or 0
NEW RA_BS_EL0     /first element addr (resolved by saku.re dispatch)
NEW RA_BS_EL1     /second element addr
NEW RA_BS_ELC     /element count parsed (also used by SWITCH macro in macros.re)

/── Autolink state ────────────────────────────────────────────
NEW BS_LAST_ITO  /addr of last ITO lux in current autolink chain (0 = none)

/── BS_LOOKUP: find token in symbol table → RA_BS_RESULT ─────
/IN:  RA_LOAD_HASH = djb2 hash of token (from BS_READ_TOKEN or BS_TOKEN_VALUE)
/OUT: RA_BS_RESULT = lux addr, or 0 if not found
NOLINK
ITO BS_LOOKUP    And   El1=RA_LOAD_HASH  El2=MASK_LOW32  Exit=RA_HT_HASH
CHAIN BS_LK_BASE
    Move  El1=BS_HT_BASE    Exit=RA_HT_BASE
    Move  El1=BS_HT_MASK    Exit=RA_HT_MASK
    Move  El1=BS_HT_SIZE    Exit=RA_HT_SIZE
RVOCA BS_LK_HT   HT_LOOKUP
ITO BS_LK_RES    Move  El1=RA_HT_RESULT  Exit=RA_BS_RESULT
RREDI BS_LK_RET

/── BS_INTERN: lookup or lazy-alloc → RA_BS_RESULT ───────────
/Plain variant: no name storage. Used for ops, el values, misc tokens.
/On miss: alloc 1 lux (lux[0] = self-ref entry point), insert into htable.
NOLINK
RVOCA BS_INTERN   BS_LOOKUP
JZ BS_INT_CK RA_BS_RESULT BS_INT_ALLOC
RREDI BS_INT_DONE
NOLINK
ALLOC_TO BS_INT_ALLOC RA_BS_RESULT C_1
ITO BS_INT_HTINS  And   El1=RA_LOAD_HASH  El2=MASK_LOW32  Exit=RA_HT_HASH
CHAIN BS_INT_HTLID
    Move  El1=RA_BS_RESULT  Exit=RA_HT_LID
    Move  El1=BS_HT_BASE    Exit=RA_HT_BASE
    Move  El1=BS_HT_MASK    Exit=RA_HT_MASK
    Move  El1=BS_HT_SIZE    Exit=RA_HT_SIZE
RVOCA BS_INT_HT   HT_INSERT
RREDI BS_INT_RETR

/── BS_INTERN_NAMED: like BS_INTERN but stores packed name ────
/Named variant: alloc 2 luces, pack token bytes into lux[1].
/Used for command tokens so Python can rebuild the symbol table from aether.
/Clobbers: RA_ALLOC_RESULT (saved/restored via a scratch slot above the
/current stack top — RA_SP+1). NOTE: this does NOT participate in the
/Voca/Redi push/pop protocol; it just borrows free memory one slot above
/RA_SP as temporary scratch. Safe only because BS_PACK_TOKBUF (called
/between save and restore) makes no nested Voca calls of its own — if it
/ever did, a nested call would itself push/pop through RA_SP without
/touching RA_SP+1, so the borrowed slot remains undisturbed either way.
NOLINK
RVOCA BS_INTERN_NAMED  BS_LOOKUP
JZ BS_INN_CK RA_BS_RESULT BS_INN_ALLOC
RREDI BS_INN_DONE
NOLINK
ITO BS_INN_ALLOC  Move  El1=C_2           Exit=RA_ALLOC_COUNT
RVOCA BS_INN_AC   ALLOC_LUCES
/Save alloc addr on call stack frame slot 1 (BS_PACK_TOKBUF clobbers RA_ALLOC_RESULT)
ITO BS_INN_SFS    Add   El1=RA_SP         El2=C_1   Exit=RA_CS_TMP
ITO BS_INN_SAV    Write El1=RA_CS_TMP    El2=RA_ALLOC_RESULT
/Pack tokbuf → packed string; result addr in RA_BS_TMP2
RVOCA BS_INN_PACK BS_PACK_TOKBUF
/Restore intern lux addr from frame slot 1
ITO BS_INN_RFS    Add   El1=RA_SP         El2=C_1   Exit=RA_CS_TMP
ITO BS_INN_RST    Read  El1=RA_CS_TMP    Exit=RA_BS_RESULT
/Store packed string addr at lux[1]
ITO BS_INN_N1A    Add   El1=RA_BS_RESULT  El2=C_1   Exit=RA_BS_TMP
ITO BS_INN_NAMEW  Write El1=RA_BS_TMP    El2=RA_BS_TMP2
/Insert into htable
ITO BS_INN_HTINS  And   El1=RA_LOAD_HASH  El2=MASK_LOW32  Exit=RA_HT_HASH
CHAIN BS_INN_HTLID
    Move  El1=RA_BS_RESULT  Exit=RA_HT_LID
    Move  El1=BS_HT_BASE    Exit=RA_HT_BASE
    Move  El1=BS_HT_MASK    Exit=RA_HT_MASK
    Move  El1=BS_HT_SIZE    Exit=RA_HT_SIZE
RVOCA BS_INN_HT   HT_INSERT
RREDI BS_INN_RETR
