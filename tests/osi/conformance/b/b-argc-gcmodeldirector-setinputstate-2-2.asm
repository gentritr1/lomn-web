; b-argc-gcmodeldirector-setinputstate-2-2
.function main2 0 @m2
.function main 0
    PushConstantf32 1.0
    PushConstantf32 0.5
err:
    CallGameFunction gcmodeldirector.setinputstate 2
    Return
m2:
    PushConstant0
    Return
