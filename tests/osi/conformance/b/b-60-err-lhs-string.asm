; b-60-err-lhs-string
.function main2 0 @m2
.function main 0
    PushConstantString "ABC"
    PushConstanti8 1
err:
    EqualTo
    Return
m2:
    PushConstant0
    Return
