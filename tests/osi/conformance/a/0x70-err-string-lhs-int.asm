.function main2 0
    PushConstanti8 1
    Return
.function main 0
    PushConstanti32 5
    PushConstantString "x"
fail:
    Add
    Return
