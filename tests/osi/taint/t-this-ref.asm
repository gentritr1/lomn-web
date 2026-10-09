; t-this-ref: an object read from an array at a tagged index is a tagged reference: its method offsets (0x12, 0x17),
; this-member reads (0x16) and the 0x51 jumps to those offsets carry the tag (the clean push/pop before 0x17
; leaves a clean stale tag in the slot it fills)
.class C props(a)
.function main 0
    CreateStackVariables 1
    CreateArray
    SetVariableValue 0
    GetVariableValue 0
    CreateObject C
    AppendToArray
    GetVariableValue 0
    CallGameFunction t.src 0
    GetArrayValue
    Dup
    GetMemberFunction .m
j1:
    JumpAbsolute 1
    Pop
    PushConstanti8 1
    Return
.method m
    GetThisMemberValue .a
b_this_val:
    CompareAndBranchIfFalse @x
x:
    PushConstanti8 1
    Pop
    GetThisMemberFunction .n
j2:
    JumpAbsolute 0
    Return
.method n
    PushConstanti8 1
    Return
