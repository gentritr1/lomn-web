; b-eng-reenter
.function main 0
.function inner 0 @inner
    PushConstanti8 11
    PushConstanti8 22
    CallGameFunction t.reenter 0
    Pull 3
    Return
inner:
    PushConstanti16 1000
    PushConstanti16 2000
    Return
