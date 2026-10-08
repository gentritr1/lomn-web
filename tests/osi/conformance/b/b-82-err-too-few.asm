; b-82-err-too-few
.function main2 0 @m2
.function main 0
    PushConstantf32 1.5
err:
    CallGameFunction t.two 1
    Return
m2:
    PushConstant0
    Return
