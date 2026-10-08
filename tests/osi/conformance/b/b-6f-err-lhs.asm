; b-6f-err-lhs
.function main2 0 @m2
.function main 0
    PushConstantString "x"
    PushConstanti8 1
err:
    BitwiseXor
    Return
m2:
    PushConstant0
    Return
