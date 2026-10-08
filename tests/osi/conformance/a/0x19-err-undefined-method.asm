.function main2 0
    PushConstanti8 1
    Return
.class C props(a)
.symbols zz
.function main 0
    CreateObject C
    PushConstantString "zz"
fail:
    GetMemberFunctionFromString
    Return
.method m
    PushConstanti8 1
    Return
