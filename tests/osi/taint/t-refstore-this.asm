; t-refstore-this: a 0x50 with a literal target passes a tagged object (a copy-kind stub's copy of it) as argument 0,
; which is `this` in the callee: its 0x15 store is through a tagged reference (`refstore`); a store through the clean
; original is not
.class C props(a)
.function main 0
    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    GetVariableValue 0
    CallGameFunction t.cpo 1
    PushConstanti32 @m
    JumpRelative 1
    Pop
    GetVariableValue 0
    PushConstanti32 @m
    JumpRelative 1
    Pop
    PushConstanti8 1
    Return
m:
    PushConstanti8 1
st:
    SetThisMemberValue .a
    PushNothing
    Return
