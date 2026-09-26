//
example.re — minimal Reca program
Defines a subroutine DOUBLE and calls it from MAIN.
After execution: RA_TMP holds 14 (7 + 7).
//

NEW X
SET X Value=7

NOLINK
NOVA DOUBLE Op=Add El1=X El2=X Exit=RA_TMP
RREDI DOUBLE_r

NOLINK
CLEAR MAIN El1=RA_TMP
RVOCA MAIN_CALL Body=DOUBLE
NOVA MAIN_END Op=End
