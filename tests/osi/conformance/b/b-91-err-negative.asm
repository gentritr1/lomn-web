; b-91-err-negative
.function main2 0 @m2
.function main 0
    CreateArray
    Dup
    PushConstanti8 7
    AppendToArray
    PushConstanti8 -1
err:
    GetArrayValue
    Return
m2:
    PushConstant0
    Return
