"""
symphony.py — Reca Aether substrate

The fundament of Reca: a flat array of u64 luces.
Python knows ONE thing: aether[0] = 0 (NULL — the absence).
Everything else is aria convention.

The canon ('Zero is absence'):
  aether[i] == 0  → nothing here
  aether[i] != 0  → something is here; what it is is decided by an aria

No special luces. No bump-allocator. No knowledge of layout.
Loader has its own cursor during bootstrap and writes the final
position into K_CURSOR.word and K_WATERMARK.word at freeze time.

Binary format (.bin, magic RECB):

  Version 3 — sparse:
    Magic:        b'RECB' (4 bytes)
    Version:      u32 LE = 3
    n_segments:   u64 LE
    For each segment:
      start_addr: u64 LE
      n_luces:    u64 LE
      data:       n_luces × u64 LE

  Version 2 — flat (legacy, still readable):
    Magic + Version=2 + n_luces + n_luces × u64

  Sparse is the default for new freezes. The flat reader is kept so
  binaries produced before the format change still load. Both freeze
  and thaw understand both formats; freeze writes only sparse.

The sparse format addresses two concerns:
  1. Luces filled with zeros are not stored, so the file size matches
     the actual content rather than the watermark range.
  2. Scatter-style allocation (writing to a high address while leaving
     low addresses empty) becomes cheap to persist — no need to dump
     the gap.
"""
import array
import struct
import sys

# Aether size: default 2M luces. Overridable via Aether(size=N) or K_AETHER_SIZE in layout.re.
# Single source of truth is aria/layout.re K_AETHER_SIZE. This default is a fallback only.
AETHER_SIZE = 1 << 21  # 2M luces — enough with named-only packing

# Target word width. Change to retarget (32 for ARM32/RV32, 64 for ARM64/RV64).
XLEN    = 64
_MASK64 = (1 << XLEN) - 1

# ── Layout constants — single source of truth ──────────────────────────
# All Python modules import from here, none define their own.

# All Lux luces share the same u64-lux Aether, but ITO and Data have distinct layouts:
#
# Data lux: [word, rel0, tgt0, rel1, tgt1, ..., 0]
#   word at slot 0, lumen pairs from slot 1, 0-terminated.
#
# ITO lux (compact fixed-slot layout, ITO_SIZE=7 base luces):
#   [0]=word(self-ref)  [1]=op  [2]=e1  [3]=e2  [4]=exit  [5]=next  [6]=pad
#   Extra LINK lumina append after slot 6 as (rel,tgt) pairs, 0-terminated.
#
# SLOT_NEXT — three execution paths:
#   line: SLOT_NEXT == 0               → next_pc = pc + ITO_SIZE  (sequential, prefetcher-friendly)
#   warp: 0 < SLOT_NEXT < FLUX_BOTTOM  → next_pc = aether[SLOT_NEXT]  (explicit address jump)
#   flux: SLOT_NEXT >= FLUX_BOTTOM     → next_pc = _exec_flux(...)  (structured parallel branch)
#
#   - Loader never writes SLOT_NEXT for sequential code → 0 → line path, zero overhead.
#   - Runtime can patch SLOT_NEXT to inject luces without moving memory.
#   - JumpIf false → SLOT_NEXT (0 = line to pc+ITO_SIZE).
#   - Jump/Redi/End are terminators and never read SLOT_NEXT.
#   - Voca: saves resolved next_pc into RA_LINK before jumping.

SLOT_WORD  = 0  # word (self-ref for ITO, value for Data)
# ITO compact layout — no rel slots, values only:
#   [0]=word  [1]=op  [2]=e1  [3]=e2  [4]=exit  [5]=next  [6]=pad  [7+]=extra LINK lumen  [...] 0
# Extra LINK lumina start at slot ITO_SIZE (=7).
# Data luces: [word, rel0, tgt0, rel1, tgt1, ..., 0] — lumen pairs scanned to 0.
# ITO and Data have distinct layouts — honest separation, no false homogeneity.
SLOT_OP    = 1  # op primitive (dispatch key)
SLOT_E1    = 2  # first operand (e1)
SLOT_E2    = 3  # second operand (e2)
SLOT_EXIT  = 4  # exit / destination
SLOT_NEXT  = 5  # next ITO: 0 = implicit fall-through (pc+ITO_SIZE), !0 = explicit graph link
ITO_SIZE   = 7  # base size: word+op+e1+e2+exit+next+pad; extra LINK lumen at addr+7+

LUMEN_PAIR    = 2  # data lumen pair = 2 luces (rel, exit)
DATA_LUX_MIN = 2  # minimal data lux = [word, 0]

# Call stack — lives at top of Aether, grows downward.
# Bump allocator grows up from low addresses; stack grows down from STACK_TOP.
# They never collide in practice (bootstrap uses ~50K luces, stack reserve = 64K).
STACK_SIZE    = 1 << 16          # 64K luces — enough for ~8K nesting levels at 8-lux frames
STACK_TOP     = AETHER_SIZE - 1  # initial SP: stack grows down from here
STACK_BOTTOM  = AETHER_SIZE - STACK_SIZE  # guard: SP must not go below this

# Flux zone — lives between bump region and call stack, grows upward.
# Flux luces have arbitrary structure described by a type lux (Data Lux).
# Detected by: addr >= FLUX_BOTTOM (one compare, only on explicit jump).
# Line path (fall-through, raw==0) never checks this — zero overhead.
# Current: simple bump allocator. Future: free-list for fragmentation.
FLUX_SIZE     = 1 << 16          # 64K luces — room for ~8K typical flux luces
FLUX_BOTTOM   = STACK_BOTTOM - FLUX_SIZE   # = 1966080; flux zone starts here
FLUX_TOP      = STACK_BOTTOM - 1           # = 2031615; flux zone ends here

# Stack frame layout (fixed, 8 luces):
#   [0] = saved RA_LINK (return address)
#   [1..7] = callee-saved registers (optional; 0 if unused)
FRAME_SIZE    = 8  # luces per call frame

# Bootstrap: first 4 cells are reserved (0=NULL, 1-3=padding)
BOOTSTRAP_FIRST_FREE = 4

# The only invariant Python enforces:
NULL_ADDR = 0   # aether[0] is always 0 — the absence

_MAGIC          = b'RECB'
_VERSION_FLAT   = 2          # legacy: dump luces [0..watermark]
_VERSION_SPARSE = 3          # current: dump only nonzero segments

# Gaps shorter than this are kept inside a segment (avoids many tiny
# headers when zeros are scattered between live luces). Longer gaps
# become segment boundaries.
_GAP_TOLERANCE = 4


class Aether:
    """One Aether instance — one flat address space.

    Python knows only that aether[0] = 0. All structure beyond that
    is convention imposed by .re ariaes (K_CURSOR, K_WATERMARK,
    K_TRACE_POS, RA_LINK, etc. are aria symbols, not magic luces).
    """

    __slots__ = ("aether", "flux_bottom", "stack_bottom")

    def __init__(self, size: int = AETHER_SIZE) -> None:
        self.aether: array.array = array.array('Q', bytes(size * 8))
        # Compute zone boundaries from actual size (not static AETHER_SIZE)
        self.stack_bottom = size - STACK_SIZE
        self.flux_bottom  = self.stack_bottom - FLUX_SIZE
        # aether[0] = 0 already — that is all Python guarantees.

    # ── Binary I/O ────────────────────────────────────────────────────────────

    def _scan_segments(self, watermark: int):
        """Find runs of nonzero luces in aether[0..watermark].

        Yields (start_addr, count) tuples. Short gaps (< _GAP_TOLERANCE
        consecutive zeros) inside a run are kept as part of the segment
        rather than splitting it; this trades a small amount of zero
        storage for fewer segment headers (which cost 16 bytes each).

        The very first lux (aether[0]) is always emitted as a
        single-lux segment if it would otherwise be skipped — this
        keeps the canon's NULL invariant explicit in the saved file.
        """
        a = self.aether
        end = watermark + 1 if watermark >= 0 else 0
        if end == 0:
            return

        # Always include lux 0 explicitly so thaw can verify the
        # NULL invariant. If lux 0 happens to be part of a longer
        # nonzero run, the main loop below extends the segment;
        # otherwise we emit just [0..1) and start scanning from 1.
        first_segment_emitted = False
        if end >= 1 and a[0] == 0:
            # Lux 0 is the canonical NULL — record it as a tiny segment
            # so the file always carries it explicitly.
            yield (0, 1)
            first_segment_emitted = True
            i = 1
        else:
            i = 0

        while i < end:
            # Skip leading zeros
            while i < end and a[i] == 0:
                i += 1
            if i >= end:
                return

            start = i
            j = i
            while j < end:
                if a[j] != 0:
                    j += 1
                    continue
                # Zero — peek ahead for tolerance
                k = j
                while k < end and a[k] == 0:
                    k += 1
                gap = k - j
                if k < end and gap < _GAP_TOLERANCE:
                    # Short gap; absorb and continue
                    j = k
                    continue
                break  # Long gap or end-of-watermark

            # Lumen case: if lux 0 is nonzero AND we haven't emitted
            # a first-segment marker, this segment starts at 0 anyway.
            yield (start, j - start)
            i = j

    def freeze(self, path: str, watermark: int) -> None:
        """Write Aether to .bin file in sparse format (version 3).

        Only nonzero segments of aether[0..watermark] are stored;
        zero ranges are omitted. The file size scales with how much
        of the Aether actually carries information, not with the
        watermark.

        watermark: the maximum address used (inclusive). The caller
        (loader) decides this — Python does not track it.
        """
        segments = list(self._scan_segments(watermark))
        with open(path, 'wb') as f:
            f.write(_MAGIC)
            f.write(struct.pack('<I', _VERSION_SPARSE))
            f.write(struct.pack('<Q', len(segments)))
            for start, count in segments:
                f.write(struct.pack('<Q', start))
                f.write(struct.pack('<Q', count))
                seg = array.array('Q', self.aether[start:start + count])
                if sys.byteorder != 'little':
                    seg.byteswap()
                seg.tofile(f)

    def thaw(self, path: str) -> None:
        """Restore Aether from .bin file (versions 2 and 3 both supported)."""
        with open(path, 'rb') as f:
            magic = f.read(4)
            if magic != _MAGIC:
                print(f"HALT: not a Reca binary (magic={magic!r})", file=sys.stderr)
                raise SystemExit(1)
            version = struct.unpack('<I', f.read(4))[0]

            if version == _VERSION_SPARSE:
                self._thaw_sparse(f)
            elif version == _VERSION_FLAT:
                self._thaw_flat(f)
            else:
                print(f"HALT: bin version {version} unsupported "
                      f"(known: {_VERSION_FLAT}, {_VERSION_SPARSE}) — "
                      "rebuild with gen_compiler.py",
                      file=sys.stderr)
                raise SystemExit(1)

    def _thaw_sparse(self, f) -> None:
        """Load sparse format (version 3)."""
        n_segs = struct.unpack('<Q', f.read(8))[0]
        for _ in range(n_segs):
            start = struct.unpack('<Q', f.read(8))[0]
            count = struct.unpack('<Q', f.read(8))[0]
            raw = f.read(count * 8)
            data = array.array('Q', raw)
            if sys.byteorder != 'little':
                data.byteswap()
            self.aether[start:start + count] = data

    def _thaw_flat(self, f) -> None:
        """Load legacy flat format (version 2)."""
        n_luces = struct.unpack('<Q', f.read(8))[0]
        raw = f.read(n_luces * 8)
        data = array.array('Q', raw)
        if sys.byteorder != 'little':
            data.byteswap()
        self.aether[:n_luces] = data

    @staticmethod
    def read_header(path: str) -> dict:
        """Inspect an .bin without fully loading it.

        Returns a dict with keys: version, format, segments (sparse only),
        luces_total (sum of luces across segments), file_size.
        Useful for `loader.py::check()`.
        """
        with open(path, 'rb') as f:
            magic = f.read(4)
            if magic != _MAGIC:
                raise ValueError(f"Not a Reca binary (magic={magic!r})")
            version = struct.unpack('<I', f.read(4))[0]
            info = {"version": version, "file_size": 0}
            import os
            info["file_size"] = os.path.getsize(path)

            if version == _VERSION_SPARSE:
                info["format"] = "sparse"
                n_segs = struct.unpack('<Q', f.read(8))[0]
                info["segments"] = n_segs
                luces_total = 0
                for _ in range(n_segs):
                    f.read(8)  # start
                    count = struct.unpack('<Q', f.read(8))[0]
                    luces_total += count
                    f.seek(count * 8, 1)  # skip data
                info["luces_total"] = luces_total
            elif version == _VERSION_FLAT:
                info["format"] = "flat"
                info["luces_total"] = struct.unpack('<Q', f.read(8))[0]
                info["segments"] = 1
            else:
                info["format"] = f"unknown(v{version})"
            return info