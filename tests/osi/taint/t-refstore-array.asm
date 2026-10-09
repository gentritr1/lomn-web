; t-refstore-array (reviewer A4): arrs = [A, B]; arrs[s] append 9 is a 0x94 store through a reference the native chose;
; count(A) then depends on s: the append is a `refstore` sink (the 0x91 index an `errpath` sink)
.function main 0
    CreateStackVariables 2
    CreateArray
    SetVariableValue 0
    CreateArray
    SetVariableValue 1
    GetVariableValue 0
    GetVariableValue 1
    AppendToArray
    GetVariableValue 0
    CreateArray
    AppendToArray
    GetVariableValue 0
    CallGameFunction t.src 0
g:
    GetArrayValue
    PushConstanti8 9
st:
    AppendToArray
    GetVariableValue 1
    ElementsInArray
b_count:
    CompareAndBranchIfFalse @e
e:
    PushConstanti8 1
    Return
