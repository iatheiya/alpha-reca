/example.re — minimal Reca program
/Defines a subroutine DOUBLE and calls it from MAIN.
/After execution: RA_TMP holds 14 (7 + 7).

NEW X
SET X 7

NOLINK
ITO DOUBLE   Add El1=X El2=X Exit=RA_TMP
RREDI DOUBLE_r

NOLINK
CLEAR MAIN RA_TMP
RVOCA MAIN_CALL DOUBLE
ITO MAIN_END End
