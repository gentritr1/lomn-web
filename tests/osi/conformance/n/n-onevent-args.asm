; n-onevent-args
.globals e0 e1 e9
.function onevent 10 @ev
.function main 0
    CallGameFunction t.addtoa 0
    Return
ev:
    GetVariableValue 0
    SetVariableValue global:e0
    GetVariableValue 1
    SetVariableValue global:e1
    GetVariableValue 9
    SetVariableValue global:e9
    PushNothing
    Return
