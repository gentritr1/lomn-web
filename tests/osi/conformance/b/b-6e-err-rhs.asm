; b-6e-err-rhs
.function main2 0 @m2
.function main 0
    PushConstanti8 1
    PushConstantf32 2.0
err:
    BitwiseOr
    Return
m2:
    PushConstant0
    Return
