.function main2 0
    PushConstanti8 1
    Return
.class C props(a, b)
.symbols zz
.function main 0
    CreateObject C
    PushConstantString "zz"
fail:
    GetMemberValueFromString
    Return
