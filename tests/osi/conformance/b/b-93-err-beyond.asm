; b-93-err-beyond
.function main2 0 @m2
.function main 0
    CreateArray
    Dup
    PushConstanti8 7
    AppendToArray
    PushConstanti8 1
    PushConstanti8 9
err:
    SetArrayValue
    Return
m2:
    PushConstant0
    Return
