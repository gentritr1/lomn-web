; t-sink-name-member-str: a 0x1a / 0x18 member name from a tagged string is a name sink too (an addition to item 5:
; the name selects the member, or the error); the value read through a tagged name carries the tag
.class C props(a)
.function main 0
    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    PushConstanti8 1
    GetVariableValue 0
    CallGameFunction t.name 0
s1a:
    SetMemberValueFromString
    GetVariableValue 0
    CallGameFunction t.name 0
s18:
    GetMemberValueFromString
b_read:
    CompareAndBranchIfFalse @a
a:
    GetVariableValue 0
    PushConstantString "a"
    GetMemberValueFromString
clean:
    CompareAndBranchIfFalse @b
b:
    PushConstanti8 1
    Return
