; t-typetests: 0xb9-0xbd type tests on a tagged value (type-only sinks), 0xbf GetObjectClassID on a tagged game handle
; and 0x92 ElementsInArray on a tagged array reference (a copy-kind stub's copy) carry the tag
.function main 0
    CallGameFunction t.src 0
    IsFloat
b_float:
    CompareAndBranchIfFalse @a
a:
    CallGameFunction t.src 0
    IsString
b_string:
    CompareAndBranchIfFalse @b
b:
    CallGameFunction t.src 0
    IsAnObject
b_object:
    CompareAndBranchIfFalse @c
c:
    CallGameFunction t.g 0
    IsGameObject
b_game:
    CompareAndBranchIfFalse @d
d:
    CallGameFunction t.src 0
    IsArray
b_array:
    CompareAndBranchIfFalse @e
e:
    CallGameFunction t.g 0
    GetObjectClassID
b_class:
    CompareAndBranchIfFalse @f
f:
    CreateArray
    CallGameFunction t.cpa 1
    ElementsInArray
b_count:
    CompareAndBranchIfFalse @g
g:
    PushConstanti8 1
    Return
