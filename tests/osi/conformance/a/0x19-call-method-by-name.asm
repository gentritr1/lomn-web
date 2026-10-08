.class C props(a)
.function main 0
    CreateObject C
    Dup
    PushConstantString "m"
    GetMemberFunctionFromString
    JumpAbsolute 1
    Return
.method m
    PushConstanti8 33
    Return
