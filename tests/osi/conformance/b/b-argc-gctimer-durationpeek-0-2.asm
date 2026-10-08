; b-argc-gctimer-durationpeek-0-2
.function main2 0 @m2
.function main 0
    PushConstanti8 9
err:
    CallGameFunction gctimer.durationpeek 0
    Return
m2:
    PushConstant0
    Return
