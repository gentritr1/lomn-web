; b-82-err-second-arg
.function main2 0 @m2
.function main 0
    PushConstanti8 3
    PushConstanti8 9
err:
    CallGameFunction t.fc 2
    Return
m2:
    PushConstant0
    Return
