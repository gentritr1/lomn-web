; t-sink-name-0x83: a 0x83 function name from a tagged string is a sink (item 5); a literal name is not; the
; result of the call carries t.dst's tag (branched on)
.function main 0
    CallGameFunction t.name 0
s83:
    CallGameFunctionFromString t 0
b_res:
    CompareAndBranchIfFalse @a
a:
    PushConstantString "dst"
    CallGameFunctionFromString t 0
    Pop
    PushConstanti8 1
    Return
