//aria/arena.re — Region-based allocation (production)

/ROLE: Bump-allocator arenas with full nesting support.

/An arena remembers the bump cursor when opened; ARENA_CLOSE rewinds
/it. Luces allocated inside become unreachable (overwritten by future
/allocs). This is "deactivation by losing connectivity."

/NESTING: fully supported via the automatic call stack (RA_SP, see registers.re).
/Each ARENA_OPEN saves cursor into the caller's frame at RA_SP+2
/(slot 0 = saved RA_LINK, written by Voca; slot 1 = reserved; slot 2 = arena mark).
/ARENA_CLOSE restores from RA_SP+2. No separate arena stack needed.

/LIMITATION: K_WATERMARK is NOT rewound on close — intentional.
/Watermark records max touch; cursor records current allocation point.

/Useful for:
/  Compiler temporaries (one arena per function compile)
/  Parser line buffers (one arena per line)
/  Test isolation (one arena per test)
/  Any bounded temporary allocation that can be discarded as a unit.

/CALLING CONVENTION: leaf, no Voca/Redi — reads/writes the caller's
/automatic call-stack frame directly (RA_SP+2). Must be called from
/inside a function that was itself entered via RVOCA (so RA_SP points
/at a live frame).

/DEPENDENCY: aspects.re  constants.re  registers.re  alloc.re  callstack.re//

/── ARENA_MARK: compute addr of arena mark in caller's frame (RA_SP + 2) ──────
/Used by ARENA_OPEN, ARENA_CLOSE, ARENA_SIZE. Leaf.//
NOLINK
    ITO ARENA_MARK  Add   El1=RA_SP  El2=C_2  Exit=RA_ALLOC_TMP
RREDI AM_RET

/── ARENA_OPEN: save cursor on call stack, open new arena region ──────────────
//Saves K_CURSOR.word into call stack frame slot 2.
/Non-leaf — caller must have entered via RVOCA (RA_SP points at live frame).
/After return, new allocations go into the arena region.//
NOLINK
    ITO ARENA_OPEN  Read  El1=K_CURSOR       Exit=RA_ALLOC_TMP2
    RVOCA AO_MARK   ARENA_MARK
    ITO AO_WMARK    Write El1=RA_ALLOC_TMP   El2=RA_ALLOC_TMP2

/── ARENA_CLOSE: restore cursor from call stack, free arena region ────────────
//Reads saved cursor from frame slot 2 and restores K_CURSOR.
/Non-leaf — same constraint as ARENA_OPEN.//
NOLINK
    RVOCA ARENA_CLOSE ARENA_MARK
    ITO AC_RDMARK   Read  El1=RA_ALLOC_TMP   Exit=RA_ALLOC_TMP2
    ITO ACL_WRCUR   Write El1=K_CURSOR        El2=RA_ALLOC_TMP2

/── ARENA_SIZE: luces allocated since last ARENA_OPEN ───────────────────────
//OUT: RA_ALLOC_RESULT = number of luces. Useful for profiling.//
NOLINK
    ITO ARENA_SIZE  Read  El1=K_CURSOR       Exit=RA_ALLOC_RESULT
    RVOCA AS_MARK   ARENA_MARK
    ITO AS_MARK2    Read  El1=RA_ALLOC_TMP   Exit=RA_ALLOC_TMP2
    ITO AS_SUB      Sub   El1=RA_ALLOC_RESULT El2=RA_ALLOC_TMP2 Exit=RA_ALLOC_RESULT
