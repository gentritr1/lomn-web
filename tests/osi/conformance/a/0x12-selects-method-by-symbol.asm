.class C props(a)
.function main 0
    CreateObject C
    Dup
    GetMemberFunction .m2
    JumpAbsolute 1
    Return
.method m1
    PushConstanti8 11
    Return
.method m2
    PushConstanti8 22
    Return
