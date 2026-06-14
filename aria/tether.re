/============================================================/
/tether.re — Graph byte-chain aria/

/PHILOSOPHY:/
/By default, byte-chains in Reca are sequences of consecutive Lux/
/(allocated by loader). This is maximally fast — traversal is just/
/Add C_1. No Lumen overhead, no graph lookup./

/This aria provides the ALTERNATIVE: byte-chains as true Lumen-graphs./
/Each Lux in the chain carries a Next Lumen → following Lux./
/This makes chains portable (GC-safe, reorderable, splittable)/
/at the cost of traversal overhead./

/The choice is the author's. Base = fast. Aria = free./
/Both coexist. You can mix them — use base chains for performance-/
/critical strings, graph chains where you need freedom./

/WHAT THIS ARIA PROVIDES:/

/CHAIN_LINK    — add Next Lumen to an existing byte-chain/
/IN:  RA_CHAIN_HEAD = first Lux ID of chain/
/Walks the chain via Add C_1 (base protocol),/
/links each Lux --Next--> following Lux./
/After: chain is traversable via both protocols./

/CHAIN_EMIT_STR — emit a graph-linked byte chain/
/IN:  RA_TW_LUX = first Lux, RA_LINK = return/
/Walks via Next Lumen (SR_WALK_ONE)./
/Use this instead of EMIT_STR_ENTRY when the chain/
/was linked via CHAIN_LINK./

/CHAIN_SPLIT   — split a chain at offset RA_CHAIN_OFF/
/IN:  RA_CHAIN_HEAD, RA_CHAIN_OFF/
/OUT: RA_CHAIN_TAIL = first Lux of second half/
/Unlinks Next at split point. Both halves remain valid./
/Requires graph-linked chain (CHAIN_LINK applied first)./

/DEPENDENCY: aspects.re, core/constants.re, runtime/layout.re,/
/runtime/regs.re, aria/symphony.re, runtime/alloc.re, aria/output.re/
/============================================================/

/── Registers ─────────────────────────────────────────────────/
NEW RA_CHAIN_HEAD   /input: first Lux of chain to process/
NEW RA_CHAIN_OFF    /input: offset for CHAIN_SPLIT/
NEW RA_CHAIN_TAIL   /output: tail Lux from CHAIN_SPLIT/
NEW RA_CHAIN_CUR    /scratch: current Lux during walk/
NEW RA_CHAIN_NXT    /scratch: next Lux (consecutive ID)/

/── CHAIN_LINK ────────────────────────────────────────────────/
/Links an existing base byte-chain with Next Lumen./
/Walks via Add C_1 (does not require Lumen to exist yet)./
/Stops at NUL byte (word = 0). NUL Lux itself gets no Next Lumen./
/Non-leaf (calls ADD_LUMEN via LINK_OP)./
NOLINK
ITO CHAIN_LINK      Move     El1=RA_CHAIN_HEAD  Exit=RA_CHAIN_CUR
ITO CHL_LOOP        Read  El1=RA_CHAIN_CUR   Exit=RA_TMP
/if word == 0 (NUL terminator): done — NUL gets no Next/
JZ CHL_NULCHK RA_TMP CHL_DONE
/compute next (consecutive ID)/
ITO CHL_NEXT        Add      El1=RA_CHAIN_CUR El2=C_1 Exit=RA_CHAIN_NXT
/link cur --Next--> nxt/
LINK_OP CHL_LNK RA_CHAIN_CUR Next RA_CHAIN_NXT
/advance/
ITO CHL_ADV         Move     El1=RA_CHAIN_NXT   Exit=RA_CHAIN_CUR
ITO CHL_LB          Jump     Exit=CHL_LOOP
RREDI CHL_DONE
/── CHAIN_EMIT_STR ────────────────────────────────────────────/
/Emit a graph-linked byte-chain (uses Next Lumen for traversal)./
/IN:  RA_TW_LUX = first Lux; RA_LINK = caller return./
/Non-leaf — return is automatic./
NOLINK
NEWREF CHAIN_EMIT_STR CES_LOOP  /alias/
ITO CES_LOOP        Read  El1=RA_TW_LUX      Exit=RA_BYTE
/NUL check/
JZ CES_NULCHK RA_BYTE CES_DONE
/emit byte/
RVOCA CES_PB PUT_BYTE
/advance via Next Lumen/
WALK_ONE CES_WALK RA_TW_LUX Next
JZ CES_AFTER_WALK RA_SR_OUT CES_DONE
ITO CES_ADV         Move      El1=RA_SR_OUT      Exit=RA_TW_LUX
ITO CES_LB          Jump      Exit=CES_LOOP
RREDI CES_DONE
/── CHAIN_SPLIT ───────────────────────────────────────────────/
/Split a graph-linked chain at byte offset RA_CHAIN_OFF./
/IN:  RA_CHAIN_HEAD = first Lux, RA_CHAIN_OFF = split offset (0-based)/
/OUT: RA_CHAIN_TAIL = first Lux of second half (= Lux at offset)/
/The Next Lumen from (offset-1) to (offset) is removed./
/Both halves remain NUL-terminated (the original NUL stays at end)./
/If offset == 0: RA_CHAIN_TAIL = RA_CHAIN_HEAD (split at head = no-op split)./
/Requires graph-linked chain. Non-leaf (calls REMOVE_LUMEN)./
NOLINK
/offset == 0 → tail is head itself/
JZ CHAIN_SPLIT RA_CHAIN_OFF CSP_ATHEAD
/walk to (offset-1)/
ITO CSP_INITC       Move     El1=RA_CHAIN_HEAD  Exit=RA_CHAIN_CUR
CLEAR CSP_INITI RA_TMP3
JEQ CSP_WALKLOOP RA_TMP3 RA_CHAIN_OFF CSP_WALK_LAND
/advance via Next Lumen/
WALK_ONE CSP_WLK RA_CHAIN_CUR Next
JZ CSP_AFTER_WALK RA_SR_OUT CSP_WALK_LAND
ITO CSP_ADVC        Move      El1=RA_SR_OUT      Exit=RA_CHAIN_CUR
ITO CSP_INCI        Add       El1=RA_TMP3 El2=C_1 Exit=RA_TMP3
ITO CSP_WLB         Jump      Exit=CSP_WALKLOOP
/at offset-1: find Next Lux (= tail), unlink/
ITO CSP_WALK_LAND       Move     El1=RA_CHAIN_CUR   Exit=RA_CHAIN_CUR
WALK_ONE CSP_FINDNXT RA_CHAIN_CUR Next
ITO CSP_TAIL_SAVE   Move     El1=RA_SR_OUT      Exit=RA_CHAIN_TAIL
/unlink: RA_CHAIN_CUR --Next--> RA_CHAIN_TAIL/
UNLINK_OP CSP_UNLNK RA_CHAIN_CUR Next RA_CHAIN_TAIL
RREDI CSP_RET_r
/offset == 0 case/
ITO CSP_ATHEAD      Move     El1=RA_CHAIN_HEAD  Exit=RA_CHAIN_TAIL
RREDI CSP_HEADRET_r