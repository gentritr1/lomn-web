.function main2 0
    PushConstanti8 1
    Return
.function main 0
    CallGameFunction t.obj 0
fail:
    GetMemberValue .a
    Return
