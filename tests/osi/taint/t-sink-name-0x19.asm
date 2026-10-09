; t-sink-name-0x19: a 0x19 method name from a tagged string is a sink (item 5); a literal name is not
.class C props(a)
.function main 0
    CreateObject C
    CallGameFunction t.name 0
s19:
    GetMemberFunctionFromString
    Pop
    CreateObject C
    PushConstantString "m"
    GetMemberFunctionFromString
    Pop
    PushConstanti8 1
    Return
.method m
    PushConstanti8 1
    Return
