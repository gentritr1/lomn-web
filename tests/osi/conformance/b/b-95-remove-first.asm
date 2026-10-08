; b-95-remove-first
.function main 0
    CreateArray
    Dup
    PushConstanti8 10
    AppendToArray
    Dup
    PushConstanti8 20
    AppendToArray
    Dup
    PushConstanti8 30
    AppendToArray
    Dup
    PushConstant0
    RemoveFromArray
    Dup
    PushConstant0
    GetArrayValue
    Return
