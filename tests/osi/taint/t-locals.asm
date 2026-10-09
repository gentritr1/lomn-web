; t-locals: the tag through 0x22 SetVariableValue, 0x24 IncrementVariable and 0x21 GetVariableValue (frame slots)
.function main 0
    CreateStackVariables 2
    CallGameFunction t.src 0
    SetVariableValue 0
    PushConstanti8 1
    SetVariableValue 1
    IncrementVariable 0
    DecrementVariable 1
    GetVariableValue 1
clean:
    CompareAndBranchIfFalse @a
a:
    GetVariableValue 0
b_local:
    CompareAndBranchIfFalse @b
b:
    PushConstanti8 1
    Return
