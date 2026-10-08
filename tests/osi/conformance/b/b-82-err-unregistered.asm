; b-82-err-unregistered
.function main2 0 @m2
.function main 0
    PushConstanti8 1
err:
    CallGameFunction t.missing 1
    Return
m2:
    PushConstant0
    Return
