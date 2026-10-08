; b-83-err-conversion
.function main2 0 @m2
.function main 0
    PushConstanti8 1
    PushConstantString "col"
err:
    CallGameFunctionFromString t 1
    Return
m2:
    PushConstant0
    Return
