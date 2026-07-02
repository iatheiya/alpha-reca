============================================================
//aria/auto_link.re — Mark loader-injected Next links

CANON: When the loader sees consecutive ITO/RVOCA/JEQ/JZ/...
lines, it wires `prev.Next = curr` automatically — this is the
"auto-Next" convenience that lets sequential code be written
without explicit Next= on every line. It is convenient but
LOSSY: after loading, a walker cannot tell which Next links
were declared by the author (semantically meaningful) and
which were injected by the loader from text adjacency
(pure layout artifact).

This aria closes that gap. When loaded, it provides the
`AutoNext` relation; the loader, if it sees this symbol,
adds an `AutoNext` lumen alongside every text-injected Next
"link" it creates.

prev --Next--> curr     // interpreter contract (slot 5)
prev --AutoNext--> curr // marker: loader-injected

Author-declared Next links (e.g. `ITO FOO Move Next=BAR`,
or the internal links inside macros like RVOCA/JEQ/JZ) get
only the slot 5 wiring, NO AutoNext lumen — they are real
graph structure, not adjacency artifacts.

CONSEQUENCES:

• The interpreter is unaffected. It only reads slot 5.

• A future dataflow walker can identify true control-flow
dependencies by following only Next lumina and ignoring
AutoNext lumina. This unlocks parallel execution where
instructions have no actual dependency, only happened
to sit on consecutive lines.

• Profilers, linters, and visualisers can show "this Next
came from text adjacency, not author intent" — useful for
debugging unexpected control flow.

• If the aria is removed, the loader stops adding AutoNext
lumina; nothing else changes. Existing graphs continue to
work; only the annotation is missing.

PHILOSOPHY:

The canon does not throw information away (see inscription.re).
AutoNext is the same principle applied to control flow: the
reason a Next lumen exists (intent vs convenience) is itself
information, and Reca preserves it.

PROVIDES: AutoNext
DEPENDENCY: aspects.re//

NEWREF AutoNext
