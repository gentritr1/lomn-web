; b-95-err-beyond
.function main2 0 @m2
.function main 0
    CreateArray
    Dup
    PushConstanti8 7
    AppendToArray
    PushConstanti8 1
err:
    RemoveFromArray
    Return
m2:
    PushConstant0
    Return
