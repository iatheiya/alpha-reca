============================================================
//aria/inscription.re — Information layer (no comments, only Lux)

CANON: There is no separate category of "comments" in Reca.
Content that is not a known command is silently ignored by the
loader. The open-close block syntax handles visual organisation.

With this aria loaded, the INSCRIPTION relation is defined.
Authors create inscription Lux explicitly using the standard
NEW + SET mechanism and bind them to code with INSCRIPTION_FOR:

NEW MyNote
SET MyNote "This describes the next function."
LINK MyNote INSCRIPTION_FOR SomeFunction

Inscriptions are not auto-created from non-command lines.
The loader does not scan for "unknown" tokens. Inscription is
purely an author-driven data structure.

USAGE for arias that want to read inscriptions:
SCAN_INSCRIPTIONS visits every Lux with an INSCRIPTION self-lumen,
calling RA_SCAN_BODY (same body protocol as SCAN_ALL_LUX).
Body input: RA_I = current inscription Lux address.

PHILOSOPHY:
Information is information. What you do with it is up to you.
The canon chooses not to act on what it does not understand.
Explicit inscription is cleaner than magic auto-capture.

PROVIDES: INSCRIPTION, INSCRIPTION_FOR, SCAN_INSCRIPTIONS
DEPENDENCY: aspects.re, aria/accord.re, aria/symphony.re,
runtime/registers.re, core/constants.re
============================================================

── Relations ───────────────────────────────────────────────
INSCRIPTION: marks a Lux as carrying free text (set on the Lux
itself as a self-lumen by the loader).
INSCRIPTION_FOR: optional link from an inscription to the Lux
it describes. Authors set this manually when binding
matters (e.g. for documentation generation).//
NEWREF INSCRIPTION
NEW INSCRIPTION_FOR


── SCAN_INSCRIPTIONS ────────────────────────────────────────
//Visits every Data Lux that has an INSCRIPTION self-lumen.
Body ABI: same as SCAN_ALL_LUX. RA_I = current inscription Lux.
Normal:     Redi (RA_SCAN_STOP unchanged = 0 → continue)
Early-exit: Move C_1 → RA_SCAN_STOP; Redi

Implemented by chaining SCAN_ALL_LUX with a per-Lux predicate that
checks for an INSCRIPTION lumen. Non-leaf.//
NEW SI_USER_BODY  /saved RA_SCAN_BODY of the caller

NOLINK
ITO SCAN_INSCRIPTIONS  Move    El1=RA_SCAN_BODY    Exit=SI_USER_BODY
ITO SI_SETBODY         Move    El1=SI_BODY         Exit=RA_SCAN_BODY
RVOCA SI_SCANJ           SCAN_ALL_LUX
ITO SI_RESTORE         Move    El1=SI_USER_BODY    Exit=RA_SCAN_BODY
RREDI SI_RET_r


── SI_BODY: predicate that calls user body iff Lux has INSCRIPTION ──
//IN: RA_I = candidate Lux address.
Looks up lumen from RA_I with relation INSCRIPTION; if present,
invokes the user-supplied body via SI_USER_BODY.//

NOLINK
ITO SI_BODY            Move      El1=RA_I           Exit=RA_SR_LUX
ITO SI_SETREL          Move      El1=INSCRIPTION    Exit=RA_SR_REL
RVOCA SI_WO              SR_WALK_ONE
/SR_WALK_ONE returns RA_SR_OUT = exit of matching lumen, or 0 if absent.
JZ SI_HASCK RA_SR_OUT SI_NO
/Has INSCRIPTION lumen → call user body with RA_I unchanged.
RVOCA SI_CALLUSER SI_USER_BODY
RREDI SI_DONE_r
NOLINK
RREDI SI_NO