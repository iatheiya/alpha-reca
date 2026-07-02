# PLAN: Universal lux mutability — `next` everywhere + `bound` marker

Status: **REJECTED** — superseded by PLAN_NEXT_ONLY.md

## Why rejected

1. `OVERFLOW_REL` is **never written** anywhere in the project (0 occurrences
   in aether at runtime). The prepass already counts exact lumen counts per
   name (`LOAD_LCOUNT_GET`, `SK_ITO_LCNT`) and allocates exactly
   `base + 2*lcount + 1` slots — the `+1` sentinel is never needed.

2. `bound` is therefore redundant — prepass already knows the sizes. Adding
   `bound` as an extra slot costs memory for zero benefit.

3. `bound` has a fundamental mismatch: meaningful only at load-time, but
   takes up permanent space in the runtime binary. Prepass knowledge is free.

The correct plan: remove the `+1` sentinel slot, keep `next` (for
mutability), move `next` to position 1 in both ITO and Data lux.
See PLAN_NEXT_ONLY.md.
