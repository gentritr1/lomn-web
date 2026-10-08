; b-95-remove-count
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
    PushConstanti8 1
    RemoveFromArray
    ElementsInArray
    Return
