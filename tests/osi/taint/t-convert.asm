; t-convert: the tag through 0xb0/0xb1/0xb2 conversions and the 0xb8 type test
.function main 0
    CallGameFunction t.src 0
    ConvertToString
    ConvertToFloat
    ConvertToInteger
b_conv:
    CompareAndBranchIfFalse @a
a:
    CallGameFunction t.src 0
    IsInteger
b_type:
    CompareAndBranchIfFalse @b
b:
    PushConstantString "5"
    ConvertToInteger
clean:
    CompareAndBranchIfFalse @c
c:
    PushConstanti8 1
    Return
