; M2a-2 T6: a synthetic Base.osi for `lomn_view --script 1` (ctest osi_a9_smoke, tools/tests/test_lomn_view_script.py).
; No game data. Natives first hit in Setup (frame -1), on frame 0 and on frame 3 (Process's counter argument, index 2),
; one of them a Beta 1.5.0 mod row, so the coverage report has rows in three frames and two tiers.
; Expected with --frames 6 and typed stubs, by hand: Setup calls getresolutionwidth and clearcinema (2 calls); every
; frame calls getresolutionheight (6 calls); frame 3 also calls cheatmenu (1 call) -> calls 9, distinct 4
; (stub 3, mod 1), first-hit order: getresolutionwidth -1, clearcinema -1, getresolutionheight 0, cheatmenu 3.
.function setup 0
    CallGameFunction gcgame.getresolutionwidth 0
    Pop
    CallGameFunction gcsaver.clearcinema 0
    Pop
    PushNothing
    Return
.function input 5
    PushNothing
    Return
.function process 3
    GetVariableValue 2
    PushConstanti8 3
    EqualTo
    CompareAndBranchIfFalse @common
    CallGameFunction gcsaver.cheatmenu 0
    Pop
common:
    CallGameFunction gcgame.getresolutionheight 0
    Pop
    PushConstanti8 1
    Return
