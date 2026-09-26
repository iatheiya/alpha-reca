/============================================================
//
aria/htable.re — Generic Hash Table

Probe-based hash table over Reca Lux.
All subroutines use native Voca/Redi (leaf — Redi via RA_LINK).

Slot format: 2 words. word0 = (hash32 << 32) | lux_id (unchanged from
before). word1 = pool_addr, pointing to a stored copy of the full name
this entry represents (see HT_NAME_POOL below). Empty = word0 == 0.
Probe: linear, by *slot index* (each slot is 2 words wide, so the
actual address step is 2, not 1). Size must be power of 2.
Mask = size-1.

Full-name comparison on every hash match (this session, BUGS.md
Pattern Z): the 32-bit stored hash alone is not enough to guarantee
identity at this project's scale (28000+ interned symbols) -- the
djb2 hash is 64 bits, but only the low 32 are stored/compared, so two
genuinely different names can share the same stored hash (confirmed
directly: "JEQ" and a still-pending, differently-named forward
reference collided this way, in comments.re, silently misrouting a
real dispatch). A hash match now only narrows the candidate slot; the
actual name's own bytes are compared before confirming a hit, the same
way any correct hash table implementation works (hash finds the
bucket, equality confirms the key) -- eliminates the risk by
construction rather than by making it merely less likely (which
widening the stored hash to, say, 48 or 64 bits would only do).

HT_LOOKUP:  IN: RA_HT_HASH, RA_HT_BASE, RA_HT_MASK, RA_HT_SIZE,
                RA_HT_NAME (addr of the name's own packed bytes),
                RA_HT_NLEN (length in bytes)
OUT: RA_HT_RESULT = found lux_id or 0
HT_INSERT:  IN: RA_HT_HASH, RA_HT_LID, RA_HT_BASE, RA_HT_MASK, RA_HT_SIZE,
                RA_HT_NAME, RA_HT_NLEN
HT_CLEAR:   IN: RA_HT_BASE, RA_HT_SIZE (now clears 2*RA_HT_SIZE words)

DEPENDENCY: aspects.re  core/constants.re (C_0, C_1, C_32)
aria/math.re (MASK_LOW32)
//

NEW RA_HT_BASE
NEW RA_HT_HASH
NEW RA_HT_SIZE
NEW RA_HT_MASK
NEW RA_HT_RESULT
NEW RA_HT_LID
NEW RA_HT_SLOT
NEW RA_HT_WORD
NEW RA_HT_WORD_SV  /saves RA_HT_WORD across HT_NAME_STORE calls,
                          /which reuse RA_HT_WORD internally. See BUGS.md
                          /Pattern Z.
NEW RA_HT_PROBE_SV  /saves RA_HT_PROBE across HT_NAME_CMP calls,
                           /which reuse RA_HT_PROBE internally. See
                           /BUGS.md Pattern Z.
NEW RA_HT_LID_SV  /saves RA_HT_LID across HT_INSERT's own call to
                          /HT_NAME_CMP, which reuses RA_HT_LID internally
                          /as scratch (HT_NC_DSTA/DSTA2/DSTR) -- the same
                          /"shared register clobbered by a called
                          /subroutine" mistake, this time for the actual
                          /lid HT_IN_WRITE needs afterward to upsert the
                          /existing entry correctly. See BUGS.md Pattern Z.
NEW RA_HT_SLOT_SV  /saves RA_HT_SLOT across HT_NAME_CMP calls (both
                           /HT_LOOKUP and HT_INSERT) -- RA_HT_SLOT is
                           /dual-purpose: the outer probe-loop's current
                           /bucket index AND HT_NAME_CMP's own per-
                           /character comparison counter (reset to 0 at
                           /its own start). The dominant root cause of the
                           /self-hosted infinite-loop regression: without
                           /this save/restore, the outer probe position
                           /was replaced by HT_NAME_CMP's own small
                           /leftover counter value on return, sending the
                           /next probe to an essentially random bucket
                           /instead of the next real one -- the linear
                           /probe sequence stopped advancing monotonically
                           /and could cycle between the same few buckets
                           /forever. See BUGS.md Pattern Z.
NEW RA_HT_STORED_H
NEW RA_HT_PROBE
NEW RA_HT_NAME  /addr of the name's own packed bytes, for full comparison/
NEW RA_HT_NLEN  /length in bytes of the name at RA_HT_NAME/

/── HT_NAME_POOL: bump-allocated storage for full name text ──
/Each stored entry: [length_word][ceil(length/8) packed-byte words].
/Grows forward from HT_NAME_POOL_BASE via HT_NAME_POOL_PTR; never
/freed (names live for the whole run, same as everything else interned).
NEW HT_NAME_POOL_BASE  Count=1048576  /1M words -- generous; names are short and
                                  /this project has tens of thousands, not
                                  /millions, of them
NEW HT_NAME_POOL_PTR In=OWN  /initialized in wax.re's own early bootstrap
                              /(LOAD_MAIN_HNPI) -- see BUGS.md Pattern Z for
                              /why the previous, in-file init instruction
                              /(never called by anything) was moved there.

/── HT_NAME_STORE: copy RA_HT_NAME[0..RA_HT_NLEN) into the pool ──
//
OUT: RA_HT_STORED_H = pool address where this name now lives
(length word first, then one aether slot per character -- matching
BS_TOKBUF_BASE's own storage convention, confirmed directly against
BS_READ_TOKEN: one character per slot, not 8 packed bytes per word).
Advances HT_NAME_POOL_PTR.
//
NOLINK
NOVA HT_NAME_STORE Op=Move El1=HT_NAME_POOL_PTR Exit=RA_HT_STORED_H
NOVA HT_NS_WLEN Op=Write El1=HT_NAME_POOL_PTR El2=RA_HT_NLEN
NOVA HT_NS_ADV1 Op=Add El1=HT_NAME_POOL_PTR El2=C_1 Exit=HT_NAME_POOL_PTR
CLEAR HT_NS_I El1=RA_HT_SLOT
JEQ HT_NS_DONE El1=RA_HT_SLOT El2=RA_HT_NLEN Exit=HT_NS_RET
NOVA HT_NS_SRCA Op=Add El1=RA_HT_NAME El2=RA_HT_SLOT Exit=RA_HT_WORD
NOVA HT_NS_SRCR Op=Read El1=RA_HT_WORD Exit=RA_HT_WORD
NOVA HT_NS_DSTA Op=Add El1=HT_NAME_POOL_PTR El2=RA_HT_SLOT Exit=RA_HT_STORED_H
NOVA HT_NS_DSTW Op=Write El1=RA_HT_STORED_H El2=RA_HT_WORD
NOVA HT_NS_INC Op=Add El1=RA_HT_SLOT El2=C_1 Exit=RA_HT_SLOT
NOVA HT_NS_LOOP Op=Jump Exit=HT_NS_DONE
NOLINK
NOVA HT_NS_RET Op=Add El1=HT_NAME_POOL_PTR El2=RA_HT_NLEN Exit=HT_NAME_POOL_PTR
NOVA HT_NS_RESULT Op=Sub El1=HT_NAME_POOL_PTR El2=RA_HT_NLEN Exit=RA_HT_STORED_H
NOVA HT_NS_RESULT2 Op=Sub El1=RA_HT_STORED_H El2=C_1 Exit=RA_HT_STORED_H
RREDI HT_NS_DONE2

/── HT_NAME_CMP: does RA_HT_NAME[0..RA_HT_NLEN) equal the name stored
/at pool address RA_HT_WORD (the entry's own word1)? ──
//
OUT: RA_HT_STORED_H = 1 if equal, 0 if not. Length-checked first
(cheap, rules out almost every real mismatch immediately); characters
compared one slot at a time only if lengths already match.
//
NOLINK
NOVA HT_NAME_CMP Op=Read El1=RA_HT_WORD Exit=RA_HT_PROBE
JEQ HT_NC_LENOK El1=RA_HT_PROBE El2=RA_HT_NLEN Exit=HT_NC_I
CLEAR HT_NC_NEQ El1=RA_HT_STORED_H
RREDI HT_NC_NEQRET
NOLINK
CLEAR HT_NC_I El1=RA_HT_SLOT
JEQ HT_NC_BYTES El1=RA_HT_SLOT El2=RA_HT_NLEN Exit=HT_NC_EQ
NOVA HT_NC_SRCA Op=Add El1=RA_HT_NAME El2=RA_HT_SLOT Exit=RA_HT_STORED_H
NOVA HT_NC_SRCR Op=Read El1=RA_HT_STORED_H Exit=RA_HT_STORED_H
NOVA HT_NC_DSTA Op=Add El1=RA_HT_WORD El2=C_1 Exit=RA_HT_LID
NOVA HT_NC_DSTA2 Op=Add El1=RA_HT_LID El2=RA_HT_SLOT Exit=RA_HT_LID
NOVA HT_NC_DSTR Op=Read El1=RA_HT_LID Exit=RA_HT_LID
JEQ HT_NC_MCK El1=RA_HT_STORED_H El2=RA_HT_LID Exit=HT_NC_INC
CLEAR HT_NC_MISS El1=RA_HT_STORED_H
RREDI HT_NC_MISSRET
NOLINK
NOVA HT_NC_INC Op=Add El1=RA_HT_SLOT El2=C_1 Exit=RA_HT_SLOT
NOVA HT_NC_LOOP Op=Jump Exit=HT_NC_BYTES
NOLINK
NOVA HT_NC_EQ Op=Move El1=C_1 Exit=RA_HT_STORED_H
RREDI HT_NC_EQRET

── HT_LOOKUP ─────────────────────────────────────────────────
/Bucket i lives at RA_HT_BASE + i*2 (word0=hash|lid, word1=&name). base is
/the NEW Count/Width region pointer (base = lux+1), so the table's own
/self-ref header word is the lux cell at base-1, before any bucket -- bucket
/0 at base+0 never touches it. No extra header slot inside the region: tables
/are declared Count=slots Width=2 exactly (a former base+2 skip, and the
/matching Count=slots+2 padding, were removed -- the skipped cells were never
/read, and the +1 under-reservation they hid once overran into the next NEW).
NOVA HT_LOOKUP Op=And El1=RA_HT_HASH El2=RA_HT_MASK Exit=RA_HT_SLOT
CLEAR HT_LK_PC El1=RA_HT_PROBE
NOVA HT_LK_ADDR Op=Mul El1=RA_HT_SLOT El2=C_2 Exit=RA_HT_WORD
NOVA HT_LK_ADDR2 Op=Add El1=RA_HT_BASE El2=RA_HT_WORD Exit=RA_HT_WORD
NOVA HT_LK_LOAD Op=Read El1=RA_HT_WORD Exit=RA_HT_STORED_H
JZ HT_LK_EZ El1=RA_HT_STORED_H Exit=HT_LK_NF
NOVA HT_LK_CMP Op=Right El1=RA_HT_STORED_H El2=C_32 Exit=RA_HT_STORED_H
JEQ HT_LK_HM El1=RA_HT_STORED_H El2=RA_HT_HASH Exit=HT_LK_PROBESV
NOVA HT_LK_NEXT Op=Add El1=RA_HT_SLOT El2=C_1 Exit=RA_HT_SLOT
NOVA HT_LK_WRAP Op=And El1=RA_HT_SLOT El2=RA_HT_MASK Exit=RA_HT_SLOT
NOVA HT_LK_PI Op=Add El1=RA_HT_PROBE El2=C_1 Exit=RA_HT_PROBE
JEQ HT_LK_FULL El1=RA_HT_PROBE El2=RA_HT_SIZE Exit=HT_LK_NF
NOVA HT_LK_LOOP Op=Jump Exit=HT_LK_ADDR
NOLINK
/Hash matched -- confirm with the real name before trusting it.
/RA_HT_PROBE saved/restored across HT_NAME_CMP the same way as
/HT_INSERT's own call, and for the same reason. See BUGS.md Pattern Z.
/HT_NAME_CMP expects RA_HT_WORD = the pool address itself (so its own
/first Read gets the stored length directly) -- computing slot1's
/*address* (word_addr+1) is not enough, the *value stored there* (the
/actual pool address) must be read out too. Missing that second Read
/was a real, previously undiagnosed bug: HT_NAME_CMP ended up comparing
/RA_HT_NLEN against slot1's own address instead of the stored length,
/which is essentially never equal, so a hash match with the real,
/existing entry always fell through to "different name" -- every
/lookup for an already-interned name silently missed and every insert
/created a fresh duplicate instead of finding the real one (confirmed
/directly: 846 separate self-hosted entries for the single name "Move").
/RA_HT_WORD saved/restored around this too (same reason, same pattern
/as HT_INSERT's own RA_HT_WORD_SV use below) -- once RA_HT_WORD holds
/the pool address for HT_NAME_CMP, the caller's own "address near
/slot0/slot1" tracking would otherwise be lost for the reload below.
NOVA HT_LK_PROBESV Op=Move El1=RA_HT_PROBE Exit=RA_HT_PROBE_SV
NOVA HT_LK_SLOTSV Op=Move El1=RA_HT_SLOT Exit=RA_HT_SLOT_SV
NOVA HT_LK_NWADDR Op=Add El1=RA_HT_WORD El2=C_1 Exit=RA_HT_WORD
NOVA HT_LK_WSAVE Op=Move El1=RA_HT_WORD Exit=RA_HT_WORD_SV
NOVA HT_LK_NWREAD Op=Read El1=RA_HT_WORD Exit=RA_HT_WORD
RVOCA HT_LK_NC Body=HT_NAME_CMP
NOVA HT_LK_PROBERS Op=Move El1=RA_HT_PROBE_SV Exit=RA_HT_PROBE
NOVA HT_LK_SLOTRS Op=Move El1=RA_HT_SLOT_SV Exit=RA_HT_SLOT
NOVA HT_LK_WREST Op=Move El1=RA_HT_WORD_SV Exit=RA_HT_WORD
JZ HT_LK_NCFAIL El1=RA_HT_STORED_H Exit=HT_LK_NEXT
NOVA HT_LK_RELOAD Op=Sub El1=RA_HT_WORD El2=C_1 Exit=RA_HT_WORD
NOVA HT_LK_RELOAD2 Op=Read El1=RA_HT_WORD Exit=RA_HT_WORD
NOVA HT_LK_FOUND Op=And El1=RA_HT_WORD El2=MASK_LOW32 Exit=RA_HT_RESULT
RREDI HT_LK_FRET
CLEAR HT_LK_NF El1=RA_HT_RESULT
RREDI HT_LK_NFRET

── HT_INSERT (upsert) ────────────────────────────────────────
//
Inserts or updates: if hash AND full name already present → overwrites
value. If hash matches but the name differs (a real collision), keeps
probing to the next slot instead of incorrectly treating it as the
same entry. Does NOT mutate RA_HT_LID (writes masked lid into RA_HT_SLOT
as scratch). Leaf (Redi via RA_LINK).
//
NOVA HT_INSERT Op=And El1=RA_HT_HASH El2=RA_HT_MASK Exit=RA_HT_SLOT
CLEAR HT_IN_PC El1=RA_HT_PROBE
NOVA HT_IN_ADDR Op=Mul El1=RA_HT_SLOT El2=C_2 Exit=RA_HT_WORD
NOVA HT_IN_ADDR2 Op=Add El1=RA_HT_BASE El2=RA_HT_WORD Exit=RA_HT_WORD
NOVA HT_IN_LOAD Op=Read El1=RA_HT_WORD Exit=RA_HT_STORED_H
/Empty slot → write new entry
JZ HT_IN_EZ El1=RA_HT_STORED_H Exit=HT_IN_WRITE
//
Not empty: reload raw word (STORED_H was overwritten by Equal)
Re-read to extract hash for upsert check
//
NOVA HT_IN_RELOAD Op=Read El1=RA_HT_WORD Exit=RA_HT_STORED_H
NOVA HT_IN_EXTH Op=Right El1=RA_HT_STORED_H El2=C_32 Exit=RA_HT_STORED_H
JEQ HT_IN_HCMP El1=RA_HT_STORED_H El2=RA_HT_HASH Exit=HT_IN_PROBESV
/Probe forward
NOVA HT_IN_NEXT Op=Add El1=RA_HT_SLOT El2=C_1 Exit=RA_HT_SLOT
NOVA HT_IN_WRAP Op=And El1=RA_HT_SLOT El2=RA_HT_MASK Exit=RA_HT_SLOT
NOVA HT_IN_PI Op=Add El1=RA_HT_PROBE El2=C_1 Exit=RA_HT_PROBE
JEQ HT_IN_FULL El1=RA_HT_PROBE El2=RA_HT_SIZE Exit=HT_IN_FULL_RET
NOVA HT_IN_LOOP Op=Jump Exit=HT_IN_ADDR
NOLINK
/Hash matched -- is this genuinely the same name, or a real collision?
/RA_HT_PROBE must be saved before calling HT_NAME_CMP and restored
/after -- HT_NAME_CMP reuses RA_HT_PROBE as its own internal scratch
/(the stored name's own length, read at HT_NAME_CMP's own very first
/instruction), the exact same "shared register clobbered by a called
/subroutine" mistake already fixed twice this session for RA_HT_WORD
/(this file) and SK_TMP (LOAD_RKVA_RESOLVE, wax.re) -- confirmed
/directly: RA_HT_PROBE held a stored name's own length (11216, a
/plausible packed-length value) instead of the real probe count
/immediately after HT_NAME_CMP returned, only 22 steps after being
/correctly reset to 0, causing HT_IN_FULL to fire immediately even
/with the real table only ~19% occupied. See BUGS.md Pattern Z.
/HT_NAME_CMP expects RA_HT_WORD = the pool address itself, not the
/*address* of slot1 (word_addr+1) -- that address must be dereferenced
/(Read) to get the actual pool address stored there first. Missing
/that Read was a real, previously undiagnosed bug, the dominant root
/cause behind most of this session's self-hosted/Python graph
/divergence: HT_NAME_CMP ended up comparing RA_HT_NLEN against slot1's
/own address instead of the stored length, essentially never equal, so
/a hash match against a real, already-inserted entry always fell
/through to "different name," silently creating a fresh duplicate on
/every insert instead of upserting the real one (confirmed directly:
/846 separate self-hosted entries for the single name "Move"). RA_HT_
/WORD saved/restored around this (RA_HT_WORD_SV -- safe to reuse here:
/this use completes before HT_IN_WRITE's own separate use of it below).
/RA_HT_LID saved/restored too -- HT_NAME_CMP also reuses RA_HT_LID as
/its own internal scratch (HT_NC_DSTA/DSTA2/DSTR), and HT_IN_WRITE
/below needs the *caller's* real lid value, untouched, to upsert the
/existing entry correctly. This one was real and unfixed until now:
/without it, HT_INSERT's own hash-and-name match path (freshly working
/after the fix above) silently overwrote a real existing entry's lid
/with leftover scratch garbage instead of the correct value.
NOVA HT_IN_PROBESV Op=Move El1=RA_HT_PROBE Exit=RA_HT_PROBE_SV
NOVA HT_IN_LIDSV Op=Move El1=RA_HT_LID Exit=RA_HT_LID_SV
NOVA HT_IN_SLOTSV Op=Move El1=RA_HT_SLOT Exit=RA_HT_SLOT_SV
NOVA HT_IN_NWADDR Op=Add El1=RA_HT_WORD El2=C_1 Exit=RA_HT_WORD
NOVA HT_IN_WSAVE2 Op=Move El1=RA_HT_WORD Exit=RA_HT_WORD_SV
NOVA HT_IN_NWREAD Op=Read El1=RA_HT_WORD Exit=RA_HT_WORD
RVOCA HT_IN_NC Body=HT_NAME_CMP
NOVA HT_IN_LIDRS Op=Move El1=RA_HT_LID_SV Exit=RA_HT_LID
NOVA HT_IN_SLOTRS Op=Move El1=RA_HT_SLOT_SV Exit=RA_HT_SLOT
NOVA HT_IN_PROBERS Op=Move El1=RA_HT_PROBE_SV Exit=RA_HT_PROBE
NOVA HT_IN_WREST2 Op=Move El1=RA_HT_WORD_SV Exit=RA_HT_WORD
NOVA HT_IN_NWBACK Op=Sub El1=RA_HT_WORD El2=C_1 Exit=RA_HT_WORD
JZ HT_IN_NCFAIL El1=RA_HT_STORED_H Exit=HT_IN_NEXT
NOVA HT_IN_SAMEJ Op=Jump Exit=HT_IN_WRITE
NOLINK
//
Write: pack (hash << 32) | (lid & MASK_LOW32) into slot0, store the
name's own text into the pool and its address into slot1.
RA_HT_WORD must be saved before calling HT_NAME_STORE and restored
after -- HT_NAME_STORE reuses RA_HT_WORD as its own internal loop
scratch (source address within the name buffer, see its own
HT_NS_SRCA/HT_NS_SRCR/HT_NS_DSTW), the same "shared register clobbered
by a called subroutine" mistake fixed for LOAD_RKVA_RESOLVE's own
callers earlier this session -- confirmed directly: RA_HT_WORD held
76 (garbage, matching HT_NAME_STORE's own internal address) instead of
slot0's real address after the call, causing HT_IN_SLOT1A/HT_IN_SLOT1W
to write through address 82 (RA_FLUX_BOTTOM's own address) instead of
the real slot1 location. See BUGS.md Pattern Z.
//
NOVA HT_IN_WRITE Op=Left El1=RA_HT_HASH El2=C_32 Exit=RA_HT_STORED_H
NOVA HT_IN_LIDMSK Op=And El1=RA_HT_LID El2=MASK_LOW32 Exit=RA_HT_SLOT
NOVA HT_IN_WVAL Op=Add El1=RA_HT_STORED_H El2=RA_HT_SLOT Exit=RA_HT_STORED_H
NOVA HT_IN_STORE Op=Write El1=RA_HT_WORD El2=RA_HT_STORED_H
NOVA HT_IN_WSAVE Op=Move El1=RA_HT_WORD Exit=RA_HT_WORD_SV
RVOCA HT_IN_NSTOR Body=HT_NAME_STORE
NOVA HT_IN_WREST Op=Move El1=RA_HT_WORD_SV Exit=RA_HT_WORD
NOVA HT_IN_SLOT1A Op=Add El1=RA_HT_WORD El2=C_1 Exit=RA_HT_SLOT
NOVA HT_IN_SLOT1W Op=Write El1=RA_HT_SLOT El2=RA_HT_STORED_H
RREDI HT_IN_RET
── HT_OVERFLOW_VEC ────────────────────────────────────────────
//
word = handler Lux ID, jumped to when HT_INSERT finds no free slot.
Default: HT_OVF_DEFAULT = End (halt). Override via:
SET HT_OVERFLOW_VEC Value=my_handler
This makes overflow handling reversible/customisable per Reca convention.
//
NEW HT_OVERFLOW_VEC
NOLINK
NOVA HT_IN_FULL_RET Op=JumpReg El1=HT_OVERFLOW_VEC
NOLINK
NOVA HT_OVF_DEFAULT Op=End
SET HT_OVERFLOW_VEC Value=HT_OVF_DEFAULT

── HT_CLEAR ──────────────────────────────────────────────────
/IN: RA_HT_BASE, RA_HT_SIZE. Leaf. Clears 2*RA_HT_SIZE words now --
/each slot is 2 words wide (word0=hash+lid, word1=name-pool pointer).
/Only word0 is ever checked for "empty" (JZ against it), but clearing
/word1 too keeps a freshly-cleared table fully zeroed, not just
/"functionally empty" -- cheap (HT_CLEAR is already a full sweep) and
/avoids leaving stale pointers around that nothing should ever read
/but that would be confusing to find in a memory dump.
NOLINK
CLEAR HT_CLEAR El1=RA_HT_SLOT
NOVA HT_CL_SZ2 Op=Mul El1=RA_HT_SIZE El2=C_2 Exit=RA_HT_PROBE
JEQ HT_CL_LOOP El1=RA_HT_SLOT El2=RA_HT_PROBE Exit=HT_CL_DONE
NOVA HT_CL_ADDR Op=Add El1=RA_HT_BASE El2=RA_HT_SLOT Exit=RA_HT_WORD
NOVA HT_CL_ZERO Op=Write El1=RA_HT_WORD El2=C_0
NOVA HT_CL_INC Op=Add El1=RA_HT_SLOT El2=C_1 Exit=RA_HT_SLOT
NOVA HT_CL_LB Op=Jump Exit=HT_CL_LOOP
RREDI HT_CL_DONE
