; h-cminus1-order: g = g * 10 + d per engine call; the digits show the in-frame order
.globals g
.function process 3 @proc
.function onevent 10 @ev
.function input 5 @in
.function main 0
    PushConstant0
    SetVariableValue global:g
    PushConstanti8 1
    Return
in:                                 ; d = 1 + (a1 == 251): VK 13 -> 1, key mapper 251 -> 2
    GetVariableValue global:g
    PushConstanti8 10
    Multiply
    GetVariableValue 1
    PushConstanti32 251
    EqualTo
    Add
    PushConstanti8 1
    Add
    SetVariableValue global:g
    PushNothing
    Return
ev:                                 ; d = 3 + 2 * (a0 == 6): mouse 26 -> 3, camera 6 -> 5
    GetVariableValue global:g
    PushConstanti8 10
    Multiply
    GetVariableValue 0
    PushConstanti8 6
    EqualTo
    PushConstanti8 2
    Multiply
    Add
    PushConstanti8 3
    Add
    SetVariableValue global:g
    PushNothing
    Return
proc:                               ; d = 4
    GetVariableValue global:g
    PushConstanti8 10
    Multiply
    PushConstanti8 4
    Add
    SetVariableValue global:g
    PushConstanti8 1
    Return
