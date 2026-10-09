; t-members: the tag through 0x10/0x11 (object members) and 0x15/0x16 (this members) of a method called by 0x51
.class C props(a, b)
.function main 0
    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    CallGameFunction t.src 0
    GetVariableValue 0
    SetMemberValue .a
    PushConstanti8 1
    GetVariableValue 0
    SetMemberValue .b
    GetVariableValue 0
    GetMemberValue .b
clean:
    CompareAndBranchIfFalse @x
x:
    GetVariableValue 0
    GetMemberValue .a
b_member:
    CompareAndBranchIfFalse @y
y:
    GetVariableValue 0
    Dup
    GetMemberFunction .m
    JumpAbsolute 1
b_this:
    CompareAndBranchIfFalse @z
z:
    PushConstanti8 1
    Return
.method m
    GetThisMemberValue .a
    SetThisMemberValue .b
    GetThisMemberValue .b
    Return
