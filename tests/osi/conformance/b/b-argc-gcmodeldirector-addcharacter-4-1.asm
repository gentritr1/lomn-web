; b-argc-gcmodeldirector-addcharacter-4-1
.function main2 0 @m2
.function main 0
    PushConstantString "n"
    PushConstanti8 1
    PushConstanti8 2
    PushConstanti8 3
err:
    CallGameFunction GcModelDirector.AddCharacter 4
    Return
m2:
    PushConstant0
    Return
