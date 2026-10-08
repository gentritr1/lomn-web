.class C props(a, b)
.function main 0

    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    PushConstanti8 6
    GetVariableValue 0
    PushConstantString "a"
    SetMemberValueFromString
    GetVariableValue 0
    GetMemberValue .a
    Return
