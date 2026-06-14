//============================================================
aria.re — Aria Protocol for Reca

An Aria in Reca is a set of Lux. Loading an Aria adds its Lux
to the Aether. If a Lux exists, it can be used. Presence IS import.

── THE MODEL ────────────────────────────────────────────────

Two Lux connect via Lumen → their intersection is a Harmony.
Multiple Harmonies → Aether (the resulting structure).

When a consumer Lux connects to an Aria:
consumer_lux --ARIA_HARMONY--> aria_entry_lux

The Harmony Lux IS the intersection point. The consumer declares
which Aria entry it intersects with via ARIA_THROUGH:
harmony_lux --ARIA_THROUGH--> specific_entry_lux

To disconnect: Unlink the ARIA_HARMONY Lumen.
The Aria Lux continues to exist. You simply stop intersecting.

── EXAMPLE ──────────────────────────────────────────────────

Aria declares its entries:
LINK my_aria_entry_a Entry Yaku
LINK my_aria_entry_b Entry Yaku

Consumer connects:
NEW my_harmony
LINK my_harmony ARIA_HARMONY my_aria_entry_a
LINK my_harmony ARIA_THROUGH my_aria_entry_a

── WHY NOT import name ───────────────────────────────────────

Traditional import requires:
- A global registry (central authority)
- String identity (name conflicts are possible)
- A discrete "loaded" state (binary: yes/no)

ARIA_HARMONY requires none of these.
Identity = Aether address (no conflicts possible).
State = Lumen existence (remove it to disconnect, trivially reversible).
Discovery = Aether traversal (no central authority needed).

── VERSIONING ────────────────────────────────────────────────

Different versions of an Aria are different Lux instances.
You point your ARIA_HARMONY to whichever entry Lux you want.
If an Aria wants to expose its version, it may link:
any_lux --ARIA_VER--> ver_byte_chain
This is optional metadata, not a protocol requirement.

── TRANSITIVITY (fractal) ───────────────────────────────────

An Aria can itself declare ARIA_HARMONY Lumen to other Aria.
This makes the system fractal:
your_lux → Harmony → aria_A entry
aria_A entry → Harmony → aria_B entry
No flat import tree. A Aether of Harmonies.

── IN v-0 (single Aether) ───────────────────────────────────

All Lux share one Aether (Python bootstrap).
ARIA_HARMONY Lumen are ordinary Lumen — they work correctly.
When v-1 introduces real Lux separation, the protocol is unchanged.
The runtime under it changes; your code does not.

── YAKU COMPILATION ─────────────────────────────────────────

The Yaku compiles a program by BFS from its entry point.
When it encounters a Harmony Lux (one with ARIA_HARMONY Lumen),
it follows ARIA_THROUGH Lumen to find the Aria entry to compile.
Aria Lux are compiled the same way as local Lux — BFS-transparent.

── NOTE ─────────────────────────────────────────────────────

aria.re is a protocol specification. Saku loads it like any other .re file.
Load it explicitly if you need the ARIA_* relation Lux in your program.//

============================================================

── ARIA RELATIONS ────────────────────────────────────────────

NEW ARIA_HARMONY
//consumer_lux --ARIA_HARMONY--> aria_entry_lux

Declares a Harmony: the intersection point between the consumer's Lux
and the Aria. The Harmony Lux IS the meeting point.
Remove this Lumen to disconnect. No other cleanup needed.

In the manifesto model:
two Lux yaku → Harmony (result of their Lumen connection)
ARIA_HARMONY is the relation that creates this Harmony point.//

NEW ARIA_THROUGH
//harmony_lux --ARIA_THROUGH--> specific_entry_lux

Specifies which entry Lux of the Aria this Harmony connects to
Multiple ARIA_THROUGH Lumen from one Harmony Lux are allowed,
giving access to multiple entry points through one Harmony.//

NEW ARIA_ENTRY
//aria_lux --ARIA_ENTRY--> entry_lux

Declares an entry point exposed by this Aria.
Optional — entry Lux are equally usable without this declaration.
Useful for discovery: traverse ARIA_ENTRY to find all exports.
Multiple ARIA_ENTRY Lumen allowed.//

NEW ARIA_DEP
//aria_lux --ARIA_DEP--> other_aria_entry_lux

Declares that this Aria has a Harmony to another Aria.
v-0: informational. v-1: auto-followed during loading.//

NEW ARIA_VER
//any_lux --ARIA_VER--> ver_byte_chain_start

Optional. Null-terminated ASCII version string.
Not required by the protocol.//

── LOAD SUBROUTINE REGISTERS ────────────────────────────────

NEW RA_LOAD_PATH
/word = Lux ID of first byte of path (NUL-terminated byte chain)

NEW RA_LOAD_RESULT
/word = entry Lux ID on success, 0 on failure

NEW RA_LOAD_RET
//word = return Lux ID (caller sets before Jump LOAD_START)

── LOAD_START (v-0 stub) ─────────────────────────────────────
Declared here as a named Lux so code can Jump to it.
Instruction Lux that implement it live in saku.re (LOAD_FILE, LOAD_DISPATCH_LINE).//
NEW LOAD_START

── aria.re SELF-DECLARATION (pattern demo) ──────────────────
//Shows how an Aria declares itself using the protocol.

Version byte chain: "0.0"//
NEWSET ARIA_RE_V0 48   /'0'
NEWSET ARIA_RE_V1 46   /'.'/
NEWSET ARIA_RE_V2 48   /'0'
NEWSET ARIA_RE_V3 0    /NUL/
LINK ARIA_RE_V0 Next ARIA_RE_V1
LINK ARIA_RE_V1 Next ARIA_RE_V2
LINK ARIA_RE_V2 Next ARIA_RE_V3
