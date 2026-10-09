; n-swallow-pops
.function main2 0 @m2
.function onevent 2 @ev
.function main 0
    PushConstanti8 11
    PushConstanti8 22
c:
    CallGameFunction t.sw 2
after_c:
    Pop
    PushConstanti8 1
    Return
ev:
    DebugOn
    Return
m2:
    PushConstanti8 9
    Return
