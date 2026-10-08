; M2a-1 T4: a synthetic script for the Process re-run / skip rule of lomn_osi_run (controller addendum; ctest osi_a6_rerun).
; Process returns int 0 ("run me again") until its pass counter g reaches a multiple of 4, then 1; on frame counter 2 it
; always returns 0, so that frame hits the 11-pass cap and the next frame is skipped. Expected over 6 frames (by hand):
; frames 0,1,4,5: 4 passes each; frame 2: 11 passes; frame 3: skipped -> total 27 over 5 frames, max 11, 1 skipped.
.globals g
.function setup 0
    PushConstant0
    SetVariableValue global:g
    PushNothing
    Return
.function input 5
    PushNothing
    Return
.function process 3
    GetVariableValue 2
    PushConstanti8 2
    EqualTo
    CompareAndBranchIfFalse @count
    PushConstant0
    Return
count:
    GetVariableValue global:g
    PushConstanti8 1
    Add
    SetVariableValue global:g
    GetVariableValue global:g
    PushConstanti8 4
    Modulus
    PushConstant0
    EqualTo
    Return
