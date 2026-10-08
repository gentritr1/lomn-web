; b-argc-gcareadirector-hide-1-3
.function main2 0 @m2
.function main 0
    PushConstantString "a"
err:
    CallGameFunction GcAreaDirector.Hide 1
    Return
m2:
    PushConstant0
    Return
