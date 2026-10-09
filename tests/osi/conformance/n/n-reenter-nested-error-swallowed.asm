; n-reenter-nested-error-swallowed
.function main2 0 @m2
.function mid 0 @mid
.function deep 0 @deep
.function main 0
    PushConstanti8 11
c:
    CallGameFunction t.sw 0
    Pop
    PushConstanti8 1
    Return
mid:
    PushConstanti8 22
ci:
    CallGameFunction t.inner 0
after_ci:
    Pop
    PushConstanti8 2
    Return
deep:
    PushConstanti8 33
bad:
    DebugOn
    Return
m2:
    PushConstanti8 9
    Return
