; b-b0-float
.function main 0
    PushConstantf32 1.5
    ConvertToString
    PushConstantString "1.500000"
    EqualTo
    Return
