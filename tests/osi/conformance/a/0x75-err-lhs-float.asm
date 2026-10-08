.function main2 0
    PushConstanti8 1
    Return
.function main 0
    PushConstantf32 1.5
    PushConstanti32 3
fail:
    Modulus
    Return
