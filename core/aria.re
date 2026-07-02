//============================================================
aria.re — Aria relations for Reca

An Aria is any program, library, or convention built on top of
the Aether. An Aria interprets luces — it assigns meaning,
defines structure, imposes behaviour.

Presence IS import. If a Lux exists in the Aether, it can be
used. There is no separate loading state, no global registry,
no string-based name resolution. Identity is Aether address.

This file declares optional relations an Aria may use to signal
its structure to other Arias or to tooling.

── ARIA_ENTRY ───────────────────────────────────────────────

aria_lux --ARIA_ENTRY--> entry_lux

Marks a Lux as a public entry point of this Aria.
Optional. Without it, any Lux is equally reachable.
Useful for discovery: traverse ARIA_ENTRY to find all exports.
Multiple ARIA_ENTRY lumina allowed.

── ARIA_DEP ─────────────────────────────────────────────────

aria_lux --ARIA_DEP--> other_aria_lux

Declares that this Aria depends on another Aria.
Optional metadata. Useful for tooling and dependency analysis.

── ARIA_VER ─────────────────────────────────────────────────

any_lux --ARIA_VER--> ver_byte_chain_start

Optional. NUL-terminated ASCII version string (byte chain).
Not required by the protocol.//

/── ARIA RELATIONS ────────────────────────────────────────────

NEW ARIA_ENTRY
//aria_lux --ARIA_ENTRY--> entry_lux
Declares a public entry point of this Aria.
Traverse ARIA_ENTRY lumina for discovery.//

NEW ARIA_DEP
//aria_lux --ARIA_DEP--> other_aria_lux
Declares a dependency on another Aria.//

NEW ARIA_VER
//any_lux --ARIA_VER--> ver_byte_chain_start
Optional version string (NUL-terminated byte chain).//
