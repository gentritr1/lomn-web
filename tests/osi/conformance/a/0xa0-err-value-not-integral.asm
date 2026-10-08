.function main2 0
    PushConstanti8 1
    Return
.function main 0
    PushConstantColor8888 0x11223344
    PushConstantf32 1.0
fail:
    SetRedValue
    Return
