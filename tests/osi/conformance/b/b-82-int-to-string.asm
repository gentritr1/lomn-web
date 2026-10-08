; b-82-int-to-string
.function main 0
    PushConstanti8 -42
    CallGameFunction t.s 1
    PushConstantString "-42"
    EqualTo
    Return
