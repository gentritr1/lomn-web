; b-94-append-strings
.function main 0
    CreateArray
    Dup
    PushConstanti8 1
    ConvertToString
    AppendToArray
    Dup
    PushConstanti8 2
    ConvertToString
    AppendToArray
    ElementsInArray
    Return
