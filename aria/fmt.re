//
aria/fmt.re — Format string output with unlimited Lux-chained elements

EMIT_FMT: walks a null-terminated byte chain, substituting:
0x01 = integer arg  0x02 = string arg  0x00 = end

Elements are a singly-linked list via FMT_ARG_NEXT_REL.
Non-leaf — RA_LINK is saved/restored automatically by the call stack.

Format string traversal: Add C_1 (base layer, fast — sequential Lux IDs).
"For" graph-based traversal use tether.re aria (CHAIN_EMIT_FMT).

Guard: if RA_FMT_ARG_CUR == 0 when an arg is expected → End.
This prevents silent reads of Aether[0] (NULL sentinel) when the
caller supplies fewer elements than the format string expects.

DEPENDENCY: aspects.re, core/constants.re, runtime/registers.re,
aria/output.re, aria/lumen.re
//

SET FMT_ARG_NEXT_REL

NEW RA_FMT_STR
NEW RA_FMT_ARGS_HEAD
NEW RA_FMT_ARG_CUR
NEW RA_FMT_BYTE

NOVA EMIT_FMT Op=Move El1=RA_FMT_ARGS_HEAD Exit=RA_FMT_ARG_CUR

NOVA FMT_LOOP Op=Read El1=RA_FMT_STR Exit=RA_FMT_BYTE
SWITCH RA_FMT_BYTE
    0  FMT_DONE
    1  FMT_IS_INT
    2  FMT_IS_STR

NOVA FMT_DIRECT Op=Move El1=RA_FMT_BYTE Exit=RA_BYTE
RVOCA FMT_PB_J Body=PUT_BYTE
NOVA FMT_ADV Op=Add El1=RA_FMT_STR El2=C_1 Exit=RA_FMT_STR
NOVA FMT_ADV_LB Op=Jump Exit=FMT_LOOP

── consume integer arg ───────────────────────────────────────
/Guard: halt if no element available
JZ FMT_IS_INT El1=RA_FMT_ARG_CUR Exit=FMT_ARG_HALT
NOVA FMT_IS_INT_RD Op=Read El1=RA_FMT_ARG_CUR Exit=RA_TMP2
WALK_ONE FI_ADV3 RA_FMT_ARG_CUR FMT_ARG_NEXT_REL
NOVA FMT_NEXT_ARG_DONE_I Op=Move El1=RA_SR_OUT Exit=RA_FMT_ARG_CUR
RVOCA FI_EMIT_J Body=EMIT_INT_ENTRY
NOVA FI_EMIT_ADV Op=Jump Exit=FMT_ADV

── consume string arg ────────────────────────────────────────
/Guard: halt if no element available
JZ FMT_IS_STR El1=RA_FMT_ARG_CUR Exit=FMT_ARG_HALT
NOVA FMT_IS_STR_RD Op=Read El1=RA_FMT_ARG_CUR Exit=RA_TW_LUX
WALK_ONE FS_ADV3 RA_FMT_ARG_CUR FMT_ARG_NEXT_REL
NOVA FMT_NEXT_ARG_DONE_S Op=Move El1=RA_SR_OUT Exit=RA_FMT_ARG_CUR
RVOCA FS_EMIT_J Body=EMIT_STR_ENTRY
NOVA FS_EMIT_ADV Op=Jump Exit=FMT_ADV

── missing element: halt ────────────────────────────────────
NOLINK
NOVA FMT_ARG_HALT Op=End

RREDI FMT_DONE
