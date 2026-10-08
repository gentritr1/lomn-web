.function main 0
    CreateStackVariables 1
    PushConstantString "a"
    PushConstantString "b"
    Add
    SetVariableValue 0
    GetVariableValue 0
    Pop
    PushNothing
    Return
