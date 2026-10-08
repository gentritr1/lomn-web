.function main2 0
    PushConstanti8 1
    Return
.globals g
.function main 0
fail:
    DecrementVariable global:g
    Return
