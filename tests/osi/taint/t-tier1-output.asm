; t-tier1-output: a tier1 native's output over-approximates: the tags of its arguments, of everything reachable from
; an object or array argument (here a clean array holding a tagged element), and of the results of the engine calls it
; makes (here inner(), which returns t.src2's value)
.function inner 0 @in
.function main 0
    CreateStackVariables 1
    CreateArray
    SetVariableValue 0
    GetVariableValue 0
    CallGameFunction t.src 0
    AppendToArray
    GetVariableValue 0
c1:
    CallGameFunction t.impl 1
b_contents:
    CompareAndBranchIfFalse @a
a:
    CallGameFunction t.nest 0
b_nested:
    CompareAndBranchIfFalse @b
b:
    PushConstanti8 1
    Return
in:
    CallGameFunction t.src2 0
    Return
