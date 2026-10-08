; b-62-err-lhs
.function main2 0 @m2
.function main 0
    PushConstantf32 1.0
    PushConstantString "x"
err:
    GreaterThan
    Return
m2:
    PushConstant0
    Return
