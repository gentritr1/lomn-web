.class C props(a, b)
.class D props(w)
.function main 0

    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    CreateObject D
    GetVariableValue 0
    PushConstantString "a"
    SetMemberValueFromString
    PushConstanti8 1
    GetVariableValue 0
    PushConstantString "a"
    SetMemberValueFromString
    GetVariableValue 0
    GetMemberValue .a
    Return
