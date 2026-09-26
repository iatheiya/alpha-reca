============================================================
//
aria/math.re — Numeric boundary values and bit masks

Common small integers (C_0..C_256, C_NEG1) live in core/constants.re.
This file provides boundary values and masks for 64-bit arithmetic
that have no place in constants.re.

NOTE: U64_MAX (2^64 - 1, all bits set) is identical in value to C_NEG1
(-1 stored as unsigned 64-bit). Use C_NEG1 wherever you need all-bits-set.

NOTE: I64_MIN (0x8000000000000000) is also the high-bit mask / sign bit.
Use I64_MIN by name; a separate MASK_HIGH_BIT alias is not needed.

DEPENDENCY: aspects.re
//
============================================================

── i64 boundary values ───────────────────────────────────────
SET I64_MAX Value=9223372036854775807 /2^63 - 1

SET I64_MIN Value=9223372036854775808 /-2^63 as uint64 (= 0x8000000000000000)

── Bit masks ─────────────────────────────────────────────────
SET MASK_LOW8 Value=255 /0xFF

SET MASK_LOW16 Value=65535 /0xFFFF

SET MASK_LOW32 Value=4294967295 /0xFFFFFFFF — used by htable.re
