.class C props(a, b)
.function main 0

    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    PushConstanti8 7
    GetVariableValue 0
    SetMemberValue .a
    GetVariableValue 0
    PushConstantString "a"
    GetMemberValueFromString
    Return
