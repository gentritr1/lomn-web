; b-96-err-nonarray
.function main2 0 @m2
.function main 0
    PushConstantf32 1.0
    PushConstant0
    PushConstanti8 9
err:
    InsertIntoArray
    Return
m2:
    PushConstant0
    Return
