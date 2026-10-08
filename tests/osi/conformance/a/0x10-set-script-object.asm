.class C props(a, b)
.function main 0

    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    PushConstanti8 42
    GetVariableValue 0
    SetMemberValue .a
    GetVariableValue 0
    GetMemberValue .a
    Return
