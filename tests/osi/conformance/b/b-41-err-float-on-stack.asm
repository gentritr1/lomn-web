; b-41-err-float-on-stack
.function main2 0 @m2
.function main 0
    PushConstantf32 2.5
err:
    PushConstanti24 0x123456
    Return
m2:
    PushConstant0
    Return
