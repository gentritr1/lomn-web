; h-mouse-every-frame: counts the OnEvent(26) calls and logs the button events
.globals n xs my ys log bs
.function process 3 @proc
.function onevent 10 @ev
.function main 0
    PushConstant0
    SetVariableValue global:n
    PushConstant0
    SetVariableValue global:xs
    PushConstant0
    SetVariableValue global:my
    PushConstant0
    SetVariableValue global:ys
    PushConstant0
    SetVariableValue global:log
    PushConstant0
    SetVariableValue global:bs
    PushConstanti8 1
    Return
ev:
    GetVariableValue 0
    PushConstanti8 26
    EqualTo
    CompareAndBranchIfFalse @button
    GetVariableValue global:n       ; n = n + 1
    PushConstanti8 1
    Add
    SetVariableValue global:n
    GetVariableValue global:xs      ; xs = xs + x
    GetVariableValue 1
    Add
    SetVariableValue global:xs
    GetVariableValue 2              ; my = y
    SetVariableValue global:my
    PushNothing
    Return
button:
    GetVariableValue global:log     ; log = log * 100 + event
    PushConstanti8 100
    Multiply
    GetVariableValue 0
    Add
    SetVariableValue global:log
    GetVariableValue global:bs      ; bs = bs * 10 + button
    PushConstanti8 10
    Multiply
    GetVariableValue 1
    Add
    SetVariableValue global:bs
    GetVariableValue global:ys      ; ys = ys + my (the y the frame's OnEvent(26) left, if it came first)
    GetVariableValue global:my
    Add
    SetVariableValue global:ys
    PushNothing
    Return
proc:
    PushConstanti8 1
    Return
