; b-83-err-too-many
.function main2 0 @m2
.function main 0
    PushConstanti8 6
    PushConstanti8 7
    PushConstantString "echo"
err:
    CallGameFunctionFromString t 2
    Return
m2:
    PushConstant0
    Return
