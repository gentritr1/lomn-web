.class C props(a, b)
.function main 0
    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    GetVariableValue 0
    GetVariableValue 0
    GetMemberFunction .m
    JumpAbsolute 1
    Return
.method m
    GetThisMemberValue .a
    Return
