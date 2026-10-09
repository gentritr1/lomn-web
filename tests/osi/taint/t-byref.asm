; t-byref: a by-ref write is a native output; it is visible to the script only after a swallow (D16 rule 3), when
; the handler's pops take the leftovers and the written argument slot stays (as in n-swallow-writes)
.function main2 0 @m2
.function onevent 1 @ev
.function main 0
    PushConstanti8 3
    CallGameFunction t.sw 1
    Pop
    PushConstanti8 1
    Return
ev:
    DebugOn
    PushConstanti8 7
    Return
m2:
    PushConstanti8 1
clean:
    CompareAndBranchIfFalse @x
x:
    Pull 2
b_byref:
    CompareAndBranchIfFalse @y
y:
    PushConstanti8 1
    Return
