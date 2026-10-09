; t-arrays: the tag through 0x94 Append, 0x96 Insert, 0x93 Set, 0x95 Remove and 0x91 Get (per element)
.function main 0
    CreateStackVariables 1
    CreateArray
    SetVariableValue 0
    GetVariableValue 0
    PushConstanti8 1
    AppendToArray
    GetVariableValue 0
    CallGameFunction t.src 0
    AppendToArray
    GetVariableValue 0
    PushConstant0
    PushConstanti8 7
    InsertIntoArray
    GetVariableValue 0
    PushConstanti8 1
    PushConstanti8 9
    SetArrayValue
    GetVariableValue 0
    PushConstant0
    RemoveFromArray
    GetVariableValue 0
    PushConstant0
    GetArrayValue
clean:
    CompareAndBranchIfFalse @a
a:
    GetVariableValue 0
    PushConstanti8 1
    GetArrayValue
b_array:
    CompareAndBranchIfFalse @b
b:
    GetVariableValue 0
    PushConstant0
    CallGameFunction t.src 0
    SetArrayValue
    GetVariableValue 0
    PushConstant0
    GetArrayValue
b_set:
    CompareAndBranchIfFalse @c
c:
    PushConstanti8 1
    Return
