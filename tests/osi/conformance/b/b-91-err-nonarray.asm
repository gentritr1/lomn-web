; b-91-err-nonarray
.function main2 0 @m2
.function main 0
    PushConstanti8 5
    PushConstant0
err:
    GetArrayValue
    Return
m2:
    PushConstant0
    Return
