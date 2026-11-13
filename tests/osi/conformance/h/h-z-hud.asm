; h-z-hud: three tier-1 ScTextObject setters, seen in the Z block
.globals o
.function process 3 @proc
.function main 0
    CallGameFunction t.text 0       ; an ScTextObject, handle 1, engine 0
    SetVariableValue global:o
    PushConstanti8 1
    Return
proc:
    GetVariableValue global:o
    PushConstantf32 400.0
    PushConstantf32 320.0
    PushConstantf32 0.0
    CallGameFunction sctextobject.setposition 4
    Pop
    GetVariableValue global:o
    PushConstantf32 1.5
    PushConstantf32 2.0
    CallGameFunction sctextobject.setscale 3
    Pop
    GetVariableValue global:o
    PushConstanti32 0x10002
    CallGameFunction sctextobject.setjustification 2
    Pop
    PushConstanti8 1
    Return
