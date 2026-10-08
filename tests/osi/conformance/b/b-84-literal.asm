; b-84-literal
.function main2 0 @m2
.function main 0
err:
    .byte 0x84 0x00 0x00 0x00 0x00 0x00
    Return
m2:
    PushConstant0
    Return
