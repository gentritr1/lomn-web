.class C props(a)
.function main 0
    CreateObject C
    Dup
    GetMemberFunction .m
    JumpAbsolute 1
    ConvertToInteger
    Return
.method m
    PushConstantString "5"
    PushConstantString "6"
    Add
    Return
