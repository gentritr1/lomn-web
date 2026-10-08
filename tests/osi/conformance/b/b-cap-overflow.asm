; b-cap-overflow
.function main2 0 @m2
.function main 0
    PushConstanti32 @f
    JumpRelative 0
f:
err:
    PushConstanti32 @f
    JumpRelative 0
ret:
m2:
    PushConstant0
    Return
