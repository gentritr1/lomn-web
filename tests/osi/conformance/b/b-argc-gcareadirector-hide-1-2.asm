; b-argc-gcareadirector-hide-1-2
.function main2 0 @m2
.function main 0
    PushConstantf32 1.5
err:
    CallGameFunction gcareadirector.hide 1
    Return
m2:
    PushConstant0
    Return
