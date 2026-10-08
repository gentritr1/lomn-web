.function main2 0
    PushConstanti8 1
    Return
.class C props(a, b)
.function main 0
    CreateObject C
    PushConstanti8 1
fail:
    GetMemberValueFromString
    Return
