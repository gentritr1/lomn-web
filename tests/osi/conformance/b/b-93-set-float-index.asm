; b-93-set-float-index
.function main 0
    CreateArray
    Dup
    PushConstanti8 7
    AppendToArray
    Dup
    PushConstantf32 0.0
    PushConstanti8 9
    SetArrayValue
    Dup
    PushConstant0
    GetArrayValue
    Return
