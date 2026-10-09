; n-reenter-nested-error-propagates
.function main2 0 @m2
.function inner 0 @inner
.function main 0
    PushConstanti8 11
c:
    CallGameFunction t.re 0
    Return
inner:
    PushConstanti8 33
bad:
    DebugOn
after_bad:
    Return
m2:
    PushConstanti8 9
    Return
