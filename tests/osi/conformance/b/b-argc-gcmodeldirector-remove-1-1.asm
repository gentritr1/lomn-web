; b-argc-gcmodeldirector-remove-1-1
.function main2 0 @m2
.function main 0
    PushConstanti8 3
err:
    CallGameFunction GcModelDirector.Remove 1
    Return
m2:
    PushConstant0
    Return
