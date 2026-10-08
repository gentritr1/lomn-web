; b-argc-gcmodeldirector-setinputstate-2-1
.function main2 0 @m2
.function main 0
    PushConstanti8 1
    PushConstanti8 2
err:
    CallGameFunction GcModelDirector.SetInputState 2
    Return
m2:
    PushConstant0
    Return
