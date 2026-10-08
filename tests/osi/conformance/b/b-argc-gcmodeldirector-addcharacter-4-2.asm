; b-argc-gcmodeldirector-addcharacter-4-2
.function main2 0 @m2
.function main 0
    PushConstanti8 7
    PushConstanti8 1
    PushConstanti8 2
    PushConstantf32 3.5
err:
    CallGameFunction gcmodeldirector.addcharacter 4
    Return
m2:
    PushConstant0
    Return
