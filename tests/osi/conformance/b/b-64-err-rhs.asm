; b-64-err-rhs
.function main2 0 @m2
.function main 0
    PushConstanti8 1
    PushConstantString "x"
err:
    GreaterOrEqual
    Return
m2:
    PushConstant0
    Return
