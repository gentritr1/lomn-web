.class C props(a, b)
.function main 0

    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    PushConstanti8 9
    GetVariableValue 0
    PushConstantString "b"
    PushConstantString ""
    Add
    SetMemberValueFromString
    GetVariableValue 0
    GetMemberValue .b
    Return
