; b-82-err-too-many
.function main2 0 @m2
.function main 0
    PushConstanti8 1
    PushConstanti8 2
err:
    CallGameFunction t.echo 2
    Return
m2:
    PushConstant0
    Return
