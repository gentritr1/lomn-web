; t-implicit: implicit flows are not propagated; the branch on the tagged value is itself the sink, and a value
; assigned under it is clean
.function main 0
    CreateStackVariables 1
    CallGameFunction t.src 0
b_src:
    CompareAndBranchIfFalse @else
    PushConstanti8 1
    SetVariableValue 0
    BranchAlways @join
else:
    PushConstanti8 2
    SetVariableValue 0
join:
    GetVariableValue 0
b_implicit:
    CompareAndBranchIfFalse @end
end:
    PushConstanti8 1
    Return
