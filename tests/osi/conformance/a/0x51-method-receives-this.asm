.class C props(a)
.function main 0
    CreateObject C
    Dup
    GetMemberFunction .m
    JumpAbsolute 1
    Return
.method m
    GetVariableValue 0
    IsAnObject
    Return
