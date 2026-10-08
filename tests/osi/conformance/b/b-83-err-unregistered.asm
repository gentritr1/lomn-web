; b-83-err-unregistered
.function main2 0 @m2
.function main 0
    PushConstanti8 6
    PushConstantString "nope"
err:
    CallGameFunctionFromString t 1
    Return
m2:
    PushConstant0
    Return
