//aria/fmt.re — Format string output with unlimited Lux-chained elements

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
aria/output.re, aria/symphony.re//

NEWREF FMT_ARG_NEXT_REL

NEW RA_FMT_STR
NEW RA_FMT_ARGS_HEAD
NEW RA_FMT_ARG_CUR
NEW RA_FMT_BYTE

ITO EMIT_FMT Move     El1=RA_FMT_ARGS_HEAD Exit=RA_FMT_ARG_CUR

ITO FMT_LOOP     Read  El1=RA_FMT_STR       Exit=RA_FMT_BYTE
SWITCH RA_FMT_BYTE
    0  FMT_DONE
    1  FMT_IS_INT
    2  FMT_IS_STR
ITO FMT_DIRECT   Move     El1=RA_FMT_BYTE      Exit=RA_BYTE
RVOCA FMT_PB_J PUT_BYTE
ITO FMT_ADV      Add      El1=RA_FMT_STR       El2=C_1 Exit=RA_FMT_STR
ITO FMT_ADV_LB   Jump     Exit=FMT_LOOP

── consume integer arg ───────────────────────────────────────
/Guard: halt if no element available
JZ FMT_IS_INT RA_FMT_ARG_CUR FMT_ARG_HALT
ITO FMT_IS_INT_RD Read   El1=RA_FMT_ARG_CUR  Exit=RA_TMP2
WALK_ONE FI_ADV3 RA_FMT_ARG_CUR FMT_ARG_NEXT_REL
ITO FMT_NEXT_ARG_DONE_I Move El1=RA_SR_OUT      Exit=RA_FMT_ARG_CUR
RVOCA FI_EMIT_J EMIT_INT_ENTRY
ITO FI_EMIT_ADV  Jump     Exit=FMT_ADV

── consume string arg ────────────────────────────────────────
/Guard: halt if no element available
JZ FMT_IS_STR RA_FMT_ARG_CUR FMT_ARG_HALT
ITO FMT_IS_STR_RD Read   El1=RA_FMT_ARG_CUR  Exit=RA_TW_LUX
WALK_ONE FS_ADV3 RA_FMT_ARG_CUR FMT_ARG_NEXT_REL
ITO FMT_NEXT_ARG_DONE_S Move El1=RA_SR_OUT      Exit=RA_FMT_ARG_CUR
RVOCA FS_EMIT_J EMIT_STR_ENTRY
ITO FS_EMIT_ADV  Jump     Exit=FMT_ADV

── missing element: halt ────────────────────────────────────
NOLINK
ITO FMT_ARG_HALT End

RREDI FMT_DONE