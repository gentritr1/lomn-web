.function main2 0
    PushConstanti8 1
    Return
.function main 0
    PushConstantf32 1.0
    PushConstantf32 bits:0x80000000
fail:
    Divide
    Return
