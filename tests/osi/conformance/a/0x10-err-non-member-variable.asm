.function main2 0
    PushConstanti8 1
    Return
.function main 0
    PushConstanti8 1
    PushConstanti8 2
fail:
    SetMemberValue .a
    Return
