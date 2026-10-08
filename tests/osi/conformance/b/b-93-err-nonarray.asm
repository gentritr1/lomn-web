; b-93-err-nonarray
.function main2 0 @m2
.function main 0
    PushConstanti8 5
    PushConstant0
    PushConstanti8 9
err:
    SetArrayValue
    Return
m2:
    PushConstant0
    Return
