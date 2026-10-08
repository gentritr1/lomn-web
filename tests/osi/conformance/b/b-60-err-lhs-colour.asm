; b-60-err-lhs-colour
.function main2 0 @m2
.function main 0
    PushConstantColor8888 0x01020304
    PushConstanti8 1
err:
    EqualTo
    Return
m2:
    PushConstant0
    Return
