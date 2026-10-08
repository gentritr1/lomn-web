; b-84-memo-ok
.function main 0
    PushConstanti8 1
    PushConstanti32 @f
    JumpRelative 1
    Pop
    PushConstanti8 2
    PushConstanti32 @f
    JumpRelative 1
    Return
f:
    CallGameFunction t.echo 1
    Return
