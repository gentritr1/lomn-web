.function main2 0
    PushConstanti8 1
    Return
.function main 0
    PushConstanti8 5
    CallGameFunction t.obj 0
    PushConstantString "a"
fail:
    SetMemberValueFromString
    Return
