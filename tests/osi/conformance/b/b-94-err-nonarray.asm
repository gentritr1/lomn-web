; b-94-err-nonarray
.function main2 0 @m2
.function main 0
    PushConstanti8 5
    PushConstanti8 1
err:
    AppendToArray
    Return
m2:
    PushConstant0
    Return
