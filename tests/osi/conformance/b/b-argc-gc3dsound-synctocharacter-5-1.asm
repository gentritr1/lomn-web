; b-argc-gc3dsound-synctocharacter-5-1
.function main2 0 @m2
.function main 0
    PushConstanti8 1
    PushConstanti8 2
    PushConstanti8 3
    PushConstanti8 4
    PushConstanti8 5
err:
    CallGameFunction Gc3DSound.SyncToCharacter 5
    Return
m2:
    PushConstant0
    Return
