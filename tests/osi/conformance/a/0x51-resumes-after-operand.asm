.class C props(a)
.function main 0
    CreateObject C
    Dup
    GetMemberFunction .m
    JumpAbsolute 1
    Pop
    PushConstanti8 2
    Return
.method m
    PushConstanti8 11
    Return
