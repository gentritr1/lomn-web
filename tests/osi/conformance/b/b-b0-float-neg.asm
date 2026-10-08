; b-b0-float-neg
.function main 0
    PushConstantf32 -0.25
    ConvertToString
    PushConstantString "-0.250000"
    EqualTo
    Return
