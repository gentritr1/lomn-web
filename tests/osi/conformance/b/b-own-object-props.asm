; b-own-object-props
.function main 0
.class Foo props(x, y)
    CreateObject Foo
    Dup
    PushConstanti8 1
    ConvertToString
    Swap
    SetMemberValue .x
    Dup
    PushConstanti8 2
    ConvertToString
    Swap
    SetMemberValue .y
    Pop
    PushConstant0
    Return
