.function main2 0
    PushConstanti8 1
    Return
.class C props(a)
.function main 0
    CreateObject C
    PushConstanti8 1
fail:
    GetMemberFunctionFromString
    Return
