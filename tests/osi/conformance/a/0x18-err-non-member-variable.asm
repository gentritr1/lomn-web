.function main2 0
    PushConstanti8 1
    Return
.function main 0
    PushConstanti8 5
    PushConstantString "a"
fail:
    GetMemberValueFromString
    Return
