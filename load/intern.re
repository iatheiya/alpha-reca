/intern.re — Symbol table and name interning for the Reca loader
/
/Contains the symbol hash table (one large contiguous region in aether),
/and the three intern operations built on top of it:
/
/  BS_LOOKUP       — find token by hash → RA_BS_RESULT (0 if absent)
/  BS_INTERN       — lookup or lazy-alloc 1-lux entry (no name stored)
/
/Names are never stored in a lux: the symtab layer owns them. The htable holds
/only (hash32 << 32) | lux_addr, so lookup is by hash and a symbol lux stays a
/clean [word, ...lumens..., 0].
/
/Also owns:
/  RA_BS_RESULT — result register shared by all three operations
/  RA_BS_EL0/EL1 — element addr scratch shared with wax.re dispatch
/  RA_BS_ELC    — element count scratch (also used by SWITCH in expand.re)
/  BS_LAST_NOVA  — autolink state: addr of last NOVA lux in current chain
/
/All callers set RA_LOAD_HASH before calling BS_LOOKUP/BS_INTERN.
/
/DEPENDS ON: aspects.re constants.re alloc.re htable.re lexer.re math.re

/── Symbol hash table ─────────────────────────────────────────
/Probe-based, linear. Slot = (hash32 << 32) | lux_addr, one luce per slot.
/Size must be a power of 2 (mask = size - 1). 65536 slots: current bootstrap
/interns ~6.7K distinct tokens, so this is ~10x headroom at ~10% load. Was
/262144 (only ~2.6% used) -- shrinking frees ~393K luces of aether, which is
/what let two full graphs (Python bootstrap + self-hosted LOAD_MAIN) coexist.
/SPAN (flat, stride 1, no per-element aliases): the table is indexed
/by (base + slot) arithmetic, so it wants one luce per slot and one name, not
/2-lux elements each with its own symbol (the alternative the now-removed
/BLOCK command would have given). Halves the table's luces and drops
/65535 useless BS_HTAB_NNNN aliases from the symbol table.
SET BS_HT_SIZE Value=65536
SET BS_HT_MASK Value=65535
NEW BS_HTAB_000  Count=65536  Width=2  /symbol table storage in aether -- 65536 slots * 2 words. HT_LOOKUP/HT_INSERT/HT_CLEAR address entry i at base+i*2 (base=lux+1, from the NEW Count/Width +1 pointer cell); no separate header -- the lux self-ref cell is the only header, and base already skips it.
/(word0=hash+lid, word1=name-pool pointer; see htable.re)

NEW BS_HT_BASE   /base lux addr of BS_HTAB (written by BS_HT_INIT, below)

/── BS_HT_INIT: initialize this file's own base pointer ────────
/Must be called once before BS_LOOKUP/BS_INTERN. Reads BS_HTAB_000's
/own self-ref word to get its lux address. Was part of a single,
/three-subsystem BS_INIT (htable/here, tokbuf/lexer.re, readbuf/
/io.re) -- split so each file initializes only its own base pointer.
/Called by LOAD_MAIN (wax.re) alongside BS_TOKBUF_INIT (lexer.re) and
/BS_READBUF_INIT (io.re).
NOVA BS_HT_INIT Op=Move El1=BS_HTAB_000 Exit=BS_HT_BASE

RREDI BS_HT_INIT_RET

/── Self-hosted symbol table (second instance of the same generic htable) ────
/htable.re's HT_LOOKUP/HT_INSERT are already parameterized by RA_HT_BASE/
/SIZE/MASK on every call -- BS_LOOKUP/BS_INTERN just feed them from
/BS_HT_BASE/SIZE/MASK. This is a second physical instance of that same
/mechanism, not a new one: same size/shape as the Python-bootstrap table
/(symmetry, not a guess), reserved and cleared by LOAD_MAIN itself before
/its own pass starts (wax.re LOAD_MAIN entry). LOAD_MAIN then repoints
/BS_HT_BASE/SIZE/MASK here for the duration of its own run -- BS_LOOKUP/
/BS_INTERN are untouched; every existing caller keeps working unchanged
/because they always resolve the table through those three registers.
/
/Why: the Python bootstrap pass and the self-hosted LOAD_MAIN pass both
/build a full graph (documented "INTENTIONAL DUPLICATION" in loader.py --
/until self-hosting is proven reliable, Python is the source of truth and
/Reca runs in parallel). Sharing one htable meant LOAD_MAIN's own
/BS_INTERN calls could find a Python-era entry for a name it's building
/for the first time itself, mistake it for "already defined, needs an
/update", and overwrite that already-correct Python-built lux's slots
/with values that only make sense in LOAD_MAIN's own address space --
/corrupting working code. Two tables make this impossible: LOAD_MAIN
/only ever sees its own entries, so it always creates fresh (which is
/exactly what "prove Reca can build this graph independently" requires).
/This is the memory-ownership/zone-isolation principle already applied to
/K_CURSOR-vs-AETHER_SIZE, now applied to the symbol table -- same shape,
/different resource. Self-erasing once Python is removed: at that point
/there is only ever one builder, so only one table is ever touched, and
/this whole second instance becomes dead weight to delete, not a
/permanent second mechanism to maintain.
NEW SK_HTAB_000  Count=65536  Width=2  /self-hosted symbol table storage in aether -- same
                            /size as BS_HTAB_000 (symmetry, not a guess); reuses
                            /BS_HT_SIZE/BS_HT_MASK directly since the value is
                            /identical, no separate SK_HT_SIZE/MASK constants
                            /(65536 slots * 2 words, entry i at base+i*2 -- see
                            /BS_HTAB_000's note and htable.re)

NEW SK_HT_BASE   /base lux addr of SK_HTAB (written by LOAD_MAIN at entry)

/── Shared result and element registers ───────────────────────
NEW RA_BS_RESULT  /result of BS_LOOKUP/BS_INTERN: lux addr or 0
NEW RA_BS_ENTRY   /result of BS_ENTRY_ADDR: word0 address of a name's HT entry
                          /(its stable identity). a[RA_BS_ENTRY] & MASK_LOW32 is
                          /the name's current location (lid), 0 until declared. See
                          /BS_ENTRY_ADDR below and LOAD_INTERN (parser.re).
NEW RA_BS_INT_COUNT  /BS_INTERN_AT: luces to allocate on miss (1 or NOVA_SIZE, set by BS_INTERN/BS_INTERN_NOVA below)
NEW RA_BS_INT_SELFREF  /BS_INTERN_AT: 1=self-ref the fresh lux on allocation, 0=leave it blank (set by BS_INTERN/BS_INTERN_NOVA below)
NEW RA_BS_INT_COMBO  /BS_INTERN_AT: region combo bitmask for this alloc (0 = flat bump). ONE-SHOT: BS_INTERN_AT captures then clears it at entry, so a caller sets it just before the call and every other caller sees 0. See alloc.re combo blocks.
NEW RA_BS_INT_COMBO_ACT  /BS_INTERN_AT internal: the captured combo for the current call
NEW RA_BS_EL0  /first element addr (resolved by wax.re dispatch)
NEW RA_BS_EL1  /second element addr
NEW RA_BS_ELC  /element count parsed (also used by SWITCH macro in expand.re)

/── Autolink state ────────────────────────────────────────────
NEW BS_LAST_NOVA  /addr of last NOVA lux in current autolink chain (0 = none)

/── BS_LOOKUP inputs: where the token being looked up lives ───
/BS_LOOKUP used to read the token straight out of BS_TOKBUF_BASE/
/RA_LOAD_TLEN. That hardcoding was the only reason parser.re could not
/call it: parser reads its tokens into its own buffer (PR_TBUF/PR_TLEN),
/so it hand-rolled a byte-for-byte copy of BS_LOOKUP's body instead --
/same table (BS_HT_BASE), same mask, same HT_LOOKUP call, differing in
/nothing but which buffer it pointed at.
/These fields make that the caller's choice. BS_LOOKUP_RESET fills
/them with the old hardcoded values, so every existing caller behaves
/exactly as before without being touched.
NEW RA_BS_NAME  /addr of the token's bytes (default: BS_TOKBUF_BASE)
NEW RA_BS_NLEN  /length of that token   (default: RA_LOAD_TLEN)
/The table itself is an input for the same reason: parser.re also looks
/names up in PBKT and LSYM, which are smaller tables of their own, and
/hand-rolled this setup twice more because BS_LOOKUP_AT could only ever
/search the shared one.
NEW RA_BS_TBASE  /table base  (default: BS_HT_BASE)
NEW RA_BS_TSIZE  /table size  (default: BS_HT_SIZE)
NEW RA_BS_TMASK  /table mask  (default: BS_HT_MASK)

/── BS_LOOKUP: find token in symbol table → RA_BS_RESULT ─────
/IN:  RA_LOAD_HASH = djb2 hash of token (from BS_READ_TOKEN or BS_TOKEN_VALUE)
/OUT: RA_BS_RESULT = lux addr, or 0 if not found
/Entry point: points RA_BS_NAME/RA_BS_NLEN at the shared token buffer,
/then runs the core. This is the old contract, unchanged -- callers that
/read their token via BS_READ_TOKEN keep calling this and need nothing new.
/A caller whose token lives elsewhere sets RA_BS_NAME/RA_BS_NLEN itself
/and calls BS_LOOKUP_AT instead.
NOLINK
NOVA BS_LOOKUP Op=Move El1=BS_TOKBUF_BASE Exit=RA_BS_NAME
NOVA BS_LK_DEFLEN Op=Move El1=RA_LOAD_TLEN Exit=RA_BS_NLEN
NOVA BS_LK_DEFTB Op=Move El1=BS_HT_BASE Exit=RA_BS_TBASE
NOVA BS_LK_DEFTS Op=Move El1=BS_HT_SIZE Exit=RA_BS_TSIZE
NOVA BS_LK_DEFTM Op=Move El1=BS_HT_MASK Exit=RA_BS_TMASK
RVOCA BS_LK_CORE Body=BS_LOOKUP_AT
RREDI BS_LK_RET

/── BS_LOOKUP_AT: same, but on the token and table the caller names ─
/IN:  RA_LOAD_HASH, RA_BS_NAME, RA_BS_NLEN,
/     RA_BS_TBASE, RA_BS_TSIZE, RA_BS_TMASK
/OUT: RA_BS_RESULT = lux addr, or 0 if not found
NOLINK
NOVA BS_LOOKUP_AT Op=And El1=RA_LOAD_HASH El2=MASK_LOW32 Exit=RA_HT_HASH
NOVA BS_LK_BASE Op=Move El1=RA_BS_TBASE Exit=RA_HT_BASE
NOVA Op=Move El1=RA_BS_TMASK Exit=RA_HT_MASK
NOVA Op=Move El1=RA_BS_TSIZE Exit=RA_HT_SIZE
NOVA Op=Move El1=RA_BS_NAME Exit=RA_HT_NAME
NOVA Op=Move El1=RA_BS_NLEN Exit=RA_HT_NLEN

RVOCA BS_LK_HT Body=HT_LOOKUP
NOVA BS_LK_RES Op=Move El1=RA_HT_RESULT Exit=RA_BS_RESULT
RREDI BS_LK_ATRET

/── BS_INTERN: lookup or lazy-alloc a 1-lux entry → RA_BS_RESULT ─
/Same two-tier shape as BS_LOOKUP/BS_LOOKUP_AT just above: sets the
/default miss-size (1 lux -- an ordinary name, never itself a NOVA
/target) into RA_BS_INT_COUNT, then falls into the shared core,
/BS_INTERN_AT. BS_INTERN_NOVA below is the other caller of that same
/core, differing only in which count it sets first -- this replaces
/what used to be two full, independently-maintained copies of the
/lookup/alloc/self-ref/HT_INSERT sequence (16 near-identical lines
/each), found while looking for a Reca-side WAX_NEW/WAX_SET-equivalent
/minimum: BS_INTERN_AT already *is* that minimum ("find-or-allocate,
/register") -- it only needed its one real difference (the miss size)
/turned into a parameter instead of a second copy.
NOLINK
NOVA BS_INTERN Op=Move El1=C_1 Exit=RA_BS_INT_COUNT
NOVA BS_INT_SR Op=Move El1=C_1 Exit=RA_BS_INT_SELFREF
RVOCA BS_INT_CORE Body=BS_INTERN_AT
RREDI BS_INT_DONE

/── BS_INTERN_NOVA: lookup-or-allocate a full NOVA_SIZE stub ────
//
Same contract as BS_INTERN, but allocates NOVA_SIZE (7) luces on miss
instead of 1. Used by the prepass (LOAD_PP_*, wax.re) to pre-size
names that will later be declared as NOVA -- so a *reference* to the
name, read before its real "NOVA name op ..." declaration is reached
(e.g. from another file, given LOAD_MAIN's interleaved scheduling),
doesn't get handed a too-small 1-lux stub that a later write past its
bounds would then clobber. Same root-cause family as the LOAD_CMD_BLOCK/
LOAD_CMD_SPAN zero-count address collisions (see BUGS.md) -- there it
was an allocation that didn't reserve enough space at all; here it's an
allocation reserving the wrong amount because, at the moment it's first
touched, nothing yet knows what the name will become.
//
NOLINK
NOVA BS_INTERN_NOVA Op=Move El1=NOVA_SIZE Exit=RA_BS_INT_COUNT
NOVA BSII_SR Op=Move El1=C_1 Exit=RA_BS_INT_SELFREF
RVOCA BSII_CORE Body=BS_INTERN_AT
RREDI BSII_DONE

/── BS_INTERN_AT: shared lookup-or-allocate-and-register core ────
/IN: RA_BS_INT_COUNT = luces to allocate on miss (set by BS_INTERN or
/BS_INTERN_NOVA above; a direct caller may set any other count itself,
/same convention as BS_LOOKUP_AT's own explicit-parameter contract).
/RA_BS_INT_SELFREF = 1 to self-ref the lux on allocation (word = its
/own address), 0 to leave it blank/zeroed. Whether to self-ref is not
/this routine's own call to make -- "find-or-allocate-and-register" is
/one question, "what goes in the freshly-allocated cell" is another,
/and the two callers below already disagree on the answer (both
/currently want 1, but PS_CMD_NEW -- parser.re's own NEW -- deliberately
/does NOT self-ref a fresh name; see loader.py's _define: "NEW's real
/meaning is a blank, zeroed lux"). Parameterized instead of decided
/here so a future caller with that same NEW-shaped need doesn't have
/to fork this routine to get it.
/On miss: alloc RA_BS_INT_COUNT luces, optionally self-ref it, insert
/into htable. Self-ref, when requested, is not optional bookkeeping --
/ALLOC/ALLOC_TO never initialize a newly-bumped lux's content
/(confirmed in alloc.re: it only advances K_CURSOR, never writes), so
/without this explicit write, the lux's word stays whatever stale
/content already happened to occupy that bump-allocated address. This
/was a real, previously undiagnosed bug: the header comment already
/promised "lux[0] = self-ref entry point," but no instruction here
/ever did it -- see BUGS.md ("forty-third round",
/check_graph_equivalence.py's first run) for how this was found: ~1900
/self-hosted op-primitive names (Move, Add, Or, ...) looked like
/garbage NOVA luces instead of clean self-ref constants. Self-ref, not
/zero: names resolved only through this path (opcode-name constants
/and similar identifiers) have no separate, later declaration that
/ever overwrites this stub -- this write IS their permanent identity,
/the same role SEAL X X plays for an ordinary command, not a temporary
/placeholder waiting to be replaced. Name storage: HT_INSERT
/(htable.re) always stores the full name text alongside the hash, for
/every table -- "no name storage" is not accurate for this function.
NOLINK
/Capture + clear the one-shot combo parameter at entry, before the lookup:
/so a hit (no alloc) can't leave it stale for the next caller, and every
/caller that did not set it sees 0 (flat bump). See alloc.re combo blocks.
NOVA BS_INTERN_AT Op=Move El1=RA_BS_INT_COMBO Exit=RA_BS_INT_COMBO_ACT
CLEAR BS_INTAT_CLRCB El1=RA_BS_INT_COMBO
RVOCA BS_INTAT_LK Body=BS_LOOKUP
JZ BS_INTAT_CK El1=RA_BS_RESULT Exit=BS_INTAT_ALLOC
RREDI BS_INTAT_DONE
NOLINK
/Miss: allocate. A region name (RA_BS_INT_COMBO_ACT != 0) comes from its
/combo's reserved block (ALLOC_COMBO); everything else from the flat bump.
NOVA BS_INTAT_ALLOC Op=Move El1=RA_BS_INT_COMBO_ACT Exit=RA_CB_TMP
JZ BS_INTAT_CBCK El1=RA_CB_TMP Exit=BS_INTAT_FLAT
NOVA BS_INTAT_CBM Op=Move El1=RA_BS_INT_COMBO_ACT Exit=RA_CB_MASK
NOVA BS_INTAT_CBN Op=Move El1=RA_BS_INT_COUNT Exit=RA_CB_N
RVOCA BS_INTAT_CBALLOC Body=ALLOC_COMBO
NOVA BS_INTAT_CBRES Op=Move El1=RA_ALLOC_RESULT Exit=RA_BS_RESULT
NOVA BS_INTAT_CBJ Op=Jump Exit=BS_INTAT_SRCK
NOLINK
ALLOC_TO BS_INTAT_FLAT Exit=RA_BS_RESULT Count=RA_BS_INT_COUNT
JZ BS_INTAT_SRCK El1=RA_BS_INT_SELFREF Exit=BS_INTAT_HTLID
NOVA BS_INTAT_SELF Op=Write El1=RA_BS_RESULT El2=RA_BS_RESULT
NOVA BS_INTAT_HTLID Op=Move El1=RA_BS_RESULT Exit=RA_HT_LID
RVOCA BS_INTAT_HT Body=BS_HT_REGISTER
RREDI BS_INTAT_RETR

/── BS_HT_REGISTER: insert (RA_LOAD_HASH, IN RA_HT_LID) into BS_HTAB ────
/IN: RA_HT_LID = the lux id/address to register -- caller sets this
/directly (BS_INTERN_AT above sets it to the just-allocated/found
/address; parser.re's PS_INS sets it to PR_EL1). RA_LOAD_HASH and the
/token buffer (BS_TOKBUF_BASE/RA_LOAD_TLEN) must already be set by the
/caller, same precondition BS_LOOKUP/BS_INTERN already have.
/Factored out of BS_INTERN_AT's own tail and parser.re's PS_INS, which
/had each independently written the same six-line HT_INSERT field
/setup for the exact same table -- found while looking for a Reca-side
/WAX_NEW/WAX_SET-equivalent minimum (the same search that led to
/BS_INTERN_AT itself): this is the "register" half of that minimum,
/and it does not need two copies any more than the rest of it did.
NOLINK
NOVA BS_HT_REGISTER Op=And El1=RA_LOAD_HASH El2=MASK_LOW32 Exit=RA_HT_HASH
NOVA Op=Move El1=BS_HT_BASE Exit=RA_HT_BASE
NOVA Op=Move El1=BS_HT_MASK Exit=RA_HT_MASK
NOVA Op=Move El1=BS_HT_SIZE Exit=RA_HT_SIZE
NOVA Op=Move El1=BS_TOKBUF_BASE Exit=RA_HT_NAME
NOVA Op=Move El1=RA_LOAD_TLEN Exit=RA_HT_NLEN
RVOCA BSHR_HT Body=HT_INSERT
RREDI BSHR_RET

/── BS_ENTRY_ADDR: find-or-create a name's HT entry, return its word0 addr ─
//
IN:  RA_LOAD_HASH + token buffer (BS_TOKBUF_BASE/RA_LOAD_TLEN), same
     precondition as BS_LOOKUP/BS_HT_REGISTER.
OUT: RA_BS_ENTRY = word0 address of the name's entry in BS_HTAB.
     a[RA_BS_ENTRY] & MASK_LOW32 is the name's location (lid) -- 0 if the
     name has no location yet ("unlocated" entry). The entry is stable
     (open-addressing entries never move once placed), so this address is
     the name's permanent identity, separate from its location.

This is how a forward reference is deferred WITHOUT a private name-pool
copy: the caller records this address once, and a later declaration
(PS_INS -> BS_HT_REGISTER -> HT_INSERT upsert, same entry by hash+name)
fills the real located lid into it. The final resolve pass then reads the
lid straight out of the entry -- no re-lookup, no stored name copy.

Passes lid=0 to the insert: reached only on a confirmed miss (the caller
looked the name up and got 0), so the entry either does not exist yet
(created here, lid 0) or exists but is itself still unlocated (lid 0
rewritten to 0, a no-op). A located name never reaches here, so this can
never clobber a real lid. HT_INSERT leaves RA_HT_WORD = the entry's word0
address in both its write and its upsert paths; capture it before it is
reused as scratch.
//
NOLINK
NOVA BS_ENTRY_ADDR Op=Move El1=C_0 Exit=RA_HT_LID
RVOCA BEA_REG Body=BS_HT_REGISTER
NOVA BEA_CAP Op=Move El1=RA_HT_WORD Exit=RA_BS_ENTRY
RREDI BEA_RET
