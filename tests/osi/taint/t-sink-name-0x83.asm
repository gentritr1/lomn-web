; t-sink-name-0x83: a 0x83 function name from a tagged string is a sink (item 5); a literal name is not
.function main 0
    CallGameFunction t.name 0
s83:
    CallGameFunctionFromString t 0
    Pop
    PushConstantString "dst"
    CallGameFunctionFromString t 0
    Pop
    PushConstanti8 1
    Return
