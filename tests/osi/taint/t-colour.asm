; t-colour: the tag through the colour channel setters 0xa0-0xa3 (a chain: each step's input is the previous result)
; and getters 0xa4-0xa7 (each branched on)
.function main 0
    CreateStackVariables 1
    CallGameFunction t.col 0
    PushConstanti8 1
    SetRedValue
    PushConstanti8 2
    SetGreenValue
    PushConstanti8 3
    SetBlueValue
    PushConstanti8 4
    SetAlphaValue
    SetVariableValue 0
    GetVariableValue 0
    GetRedValue
b_r:
    CompareAndBranchIfFalse @a
a:
    GetVariableValue 0
    GetGreenValue
b_g:
    CompareAndBranchIfFalse @b
b:
    GetVariableValue 0
    GetBlueValue
b_b:
    CompareAndBranchIfFalse @c
c:
    GetVariableValue 0
    GetAlphaValue
b_a:
    CompareAndBranchIfFalse @d
d:
    PushConstantColor8888 0x01020304
    GetRedValue
clean:
    CompareAndBranchIfFalse @e
e:
    PushConstanti8 1
    Return
