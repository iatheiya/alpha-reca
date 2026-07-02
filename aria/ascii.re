//============================================================
//aria/ascii.re — ASCII character constants

//Reca stores characters as i64 (low 8 bits = the byte).
SYS_WRITE reads the low byte of each Lux word.
These constants give human names to the values used in
yaku.re and other Reca programs.

The LC_* prefix has been removed. Names are canonical.
All values are standard 7-bit ASCII (0–127).//

── Control characters ────────────────────────────────────────
NEWSET NUL 0             /null terminator — marks end of byte chains

NEWSET TAB 9             /horizontal tab

NEWSET LF 10             /line feed / newline

NEWSET CR 13             /carriage return/

NEWSET ESC 27            /escape/

NEWSET SP 32             /space/

NEWSET DEL 127           /delete/

── Punctuation ───────────────────────────────────────────────
NEWSET MINUS 45          /-/

NEWSET PERCENT 37        /% — used in LLVM IR (%vreg, %label)/

NEWSET DQUOTE 34         /double-quote char (34)/

NEWSET HASH 35           /#/


NEWSET SLASH 47          ///

NEWSET COLON 58          /:/

NEWSET SEMICOLON 59      /;/

NEWSET EQUALS 61         /=/

NEWSET BACKSLASH 92      /backslash char (92)/

NEWSET UNDERSCORE 95     /_/
NEWSET ASCII_PIPE 124    /|/

NEWSET QMARK 63          /?/

NEWSET LBRACE 123        /{ — used in LLVM IR function bodies/

NEWSET RBRACE 125        /} — used in LLVM IR function bodies/

NEWSET TILDE 126         /~ (tilde char; was used as newline placeholder in templates, now replaced by automatic RO_NEWLINE)

/── Digits ───────────────────────────────────────────────────/
NEWSET ASCII_0 48        /'0'  (C_48 in compiler = ASCII_0)/

NEWSET ASCII_9 57        /'9'/

/── Uppercase letters ─────────────────────────────────────────/
NEWSET ASCII_A 65        /'A'/

NEWSET ASCII_L 76        /'L' — used as LLVM label prefix/

NEWSET ASCII_Z 90        /'Z'/

/── Lowercase letters ─────────────────────────────────────────/
NEWSET ASCII_a 97        /'a'/

NEWSET ASCII_z 122       /'z'/

/── Case conversion ───────────────────────────────────────────/
NEWSET CASE_DELTA 32     /add to uppercase → lowercase/

NEWSET DOT 46            /./

/── Additional uppercase letters (for parser.re command dispatch) ──/
NEWSET ASCII_B 66

NEWSET ASCII_D 68

NEWSET ASCII_F 70

NEWSET ASCII_I 73

NEWSET ASCII_N 78

NEWSET ASCII_O 79

NEWSET ASCII_Q 81

NEWSET ASCII_R 82

NEWSET ASCII_S 83

NEWSET ASCII_V 86

NEWSET ASCII_C 67

NEWSET ASCII_E 69

NEWSET ASCII_G 71

NEWSET ASCII_H 72

NEWSET ASCII_J 74

NEWSET ASCII_K 75

NEWSET ASCII_M 77

NEWSET ASCII_P 80

NEWSET ASCII_T 84

NEWSET ASCII_U 85

NEWSET ASCII_W 87

NEWSET ASCII_X 88

/── Lowercase letters (suffix 'l' = lowercase) ────────────────/
NEWSET ASCII_cl 99      /'c'/
NEWSET ASCII_dl 100     /'d'/
NEWSET ASCII_fl 102     /'f'/
NEWSET ASCII_hl 104     /'h'/
NEWSET ASCII_il 105     /'i'/
NEWSET ASCII_ll 108     /'l'/
NEWSET ASCII_nl 110     /'n'/
NEWSET ASCII_pl 112     /'p'/
NEWSET ASCII_rl 114     /'r'/
NEWSET ASCII_tl 116     /'t'/
NEWSET ASCII_vl 118     /'v'/
NEWSET ASCII_xl 120     /'x'/

/── Single digit ASCII values ──────────────────────────────────/
NEWSET ASCII_1 49       /'1'/
