; t-refstore-member (reviewer A3): arr = [o0, o1]; arr[s].a = 1 is a 0x10 store through a reference the native chose.
; o0.a, read later through a clean reference, is 1 or nothing depending on s: the store is a `refstore` sink (and the
; tagged index of the 0x91 an `errpath` sink); the value stored is clean, so the later branch is no sink
.class C props(a)
.function main 0
    CreateStackVariables 2
    CreateArray
    SetVariableValue 0
    CreateObject C
    SetVariableValue 1
    GetVariableValue 0
    GetVariableValue 1
    AppendToArray
    GetVariableValue 0
    CreateObject C
    AppendToArray
    PushConstanti8 1
    GetVariableValue 0
    CallGameFunction t.src 0
g:
    GetArrayValue
st:
    SetMemberValue .a
    GetVariableValue 1
    GetMemberValue .a
    PushNothing
    EqualTo
b_alias:
    CompareAndBranchIfFalse @e
e:
    PushConstanti8 1
    Return
