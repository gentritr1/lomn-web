.function main2 0
    PushConstanti8 1
    Return
.function main 0
    PushConstantString "x"
    PushConstanti32 5
fail:
    Add
    Return
