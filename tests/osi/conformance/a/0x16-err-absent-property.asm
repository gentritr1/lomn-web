.function main2 0
    PushConstanti8 1
    Return
.class C props(a, b)
.symbols zz
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
    PushConstanti8 77
fail:
    GetThisMemberValue .zz
    Return
