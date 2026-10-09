; n-name-256
.strings "t"
.function main2 0 @m2
.function main 0
    CallGameFunction t.long 0
c:
    CallGameFunctionFromString t 0
    Return
m2:
    PushConstanti8 9
    Return
