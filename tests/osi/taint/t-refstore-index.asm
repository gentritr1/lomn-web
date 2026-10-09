; t-refstore-index: 0x93 set, 0x96 insert and 0x95 remove at a tagged index store into an element the native chose:
; `refstore` sinks, and `errpath` sinks (the index decides the out-of-range error); the same ops at a literal index are
; neither. A tagged array index read (0x91) and tagged divisors (0x73, 0x75) are `errpath` sinks
.function main 0
    CreateStackVariables 1
    CreateArray
    SetVariableValue 0
    GetVariableValue 0
    PushConstanti8 5
    AppendToArray
    GetVariableValue 0
    PushConstanti8 6
    AppendToArray
    GetVariableValue 0
    CallGameFunction t.src 0
    PushConstanti8 7
set:
    SetArrayValue
    GetVariableValue 0
    CallGameFunction t.src 0
    PushConstanti8 8
ins:
    InsertIntoArray
    GetVariableValue 0
    CallGameFunction t.src 0
rem:
    RemoveFromArray
    GetVariableValue 0
    CallGameFunction t.src 0
get:
    GetArrayValue
    Pop
    GetVariableValue 0
    PushConstant0
    PushConstanti8 1
    SetArrayValue
    GetVariableValue 0
    PushConstant0
    GetArrayValue
    Pop
    PushConstanti8 9
    CallGameFunction t.src 0
    PushConstanti8 1
    Add
div:
    Divide
    Pop
    PushConstanti8 9
    CallGameFunction t.src 0
    PushConstanti8 2
    Add
mod:
    Modulus
    Pop
    PushConstanti8 1
    Return
