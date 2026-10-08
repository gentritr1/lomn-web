.class C props(a, b)
.function main 0

    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    PushConstanti8 8
    GetVariableValue 0
    SetMemberValue .b
    GetVariableValue 0
    PushConstantString "b"
    PushConstantString ""
    Add
    GetMemberValueFromString
    Return
