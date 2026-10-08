.class C props(a, b)
.function main 0

    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    PushConstanti8 3
    GetVariableValue 0
    SetMemberValue .a
    PushConstanti8 5
    GetVariableValue 0
    SetMemberValue .b
    GetVariableValue 0
    GetMemberValue .b
    Return
