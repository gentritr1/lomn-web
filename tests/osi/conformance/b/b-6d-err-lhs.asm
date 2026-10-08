; b-6d-err-lhs
.function main2 0 @m2
.function main 0
    PushConstantf32 1.0
    PushConstanti8 1
err:
    BitwiseAnd
    Return
m2:
    PushConstant0
    Return
