; n-swallow-reerror
.function main2 0 @m2
.function onevent 0 @ev
.function main 0
c:
    CallGameFunction t.sw 0
    PushConstanti8 1
    Return
ev:
    .byte 0x85
    Return
m2:
    PushConstanti8 9
    Return
