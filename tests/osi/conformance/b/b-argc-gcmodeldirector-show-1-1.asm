; b-argc-gcmodeldirector-show-1-1
.function main2 0 @m2
.function main 0
    PushConstantString "x"
err:
    CallGameFunction GcModelDirector.Show 1
    Return
m2:
    PushConstant0
    Return
