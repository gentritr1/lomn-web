; t-globals: the tag through 0x22/0x21 on globals, across two engine calls; 0x81 is a no-op (nothing written or popped)
; (main2 is declared first: FindFunction matches over the entry's length, so `main` would answer "main2")
.globals g h
.function main2 0 @m2
.function main 0
    CallGameFunction t.src 0
    SetVariableValue global:g
    PushConstanti8 1
    SetVariableValue global:h
    CallGameFunction t.src 0
    SetGameVariable cls.var
b_after_gamevar:
    CompareAndBranchIfFalse @a
a:
    PushConstanti8 1
    Return
m2:
    GetVariableValue global:h
clean:
    CompareAndBranchIfFalse @x
x:
    GetVariableValue global:g
b_global:
    CompareAndBranchIfFalse @y
y:
    PushConstanti8 1
    Return
