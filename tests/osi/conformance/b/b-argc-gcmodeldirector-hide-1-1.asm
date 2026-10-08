; b-argc-gcmodeldirector-hide-1-1
.function main2 0 @m2
.function main 0
    PushConstantf32 2.5
err:
    CallGameFunction GcModelDirector.Hide 1
    Return
m2:
    PushConstant0
    Return
