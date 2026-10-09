; n-swallow-writes
.function main2 0 @m2
.function onevent 1 @ev
.function main 0
    PushConstanti8 3
c:
    CallGameFunction t.sw 1
after_c:
    Pop
    PushConstanti8 1
    Return
ev:
    DebugOn
    PushConstanti8 7
    Return
m2:
    PushConstanti8 9
    Return
