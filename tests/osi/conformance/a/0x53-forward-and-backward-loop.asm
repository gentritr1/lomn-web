.function main 0
    CreateStackVariables 2
    PushConstanti8 3
    SetVariableValue 0
    PushConstant0
    SetVariableValue 1
top:
    GetVariableValue 0
    CompareAndBranchIfFalse @done
    DecrementVariable 0
    IncrementVariable 1
    BranchAlways @top
done:
    GetVariableValue 1
    Return
