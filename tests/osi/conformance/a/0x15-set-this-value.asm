.class C props(a, b)
.class D props(w)
.function main 0
    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    GetVariableValue 0
    GetVariableValue 0
    GetMemberFunction .m
    JumpAbsolute 1
    Pop
    GetVariableValue 0
    GetMemberValue .a
    Return
.method m
    PushConstanti8 5
    SetThisMemberValue .a
    PushNothing
    Return
