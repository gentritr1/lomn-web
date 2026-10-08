; b-argc-gc3dsound-synctoobject-5-1
.function main2 0 @m2
.function main 0
    PushNothing
    PushConstanti8 2
    PushConstanti8 3
    PushConstanti8 4
    PushConstantf32 5.0
err:
    CallGameFunction Gc3DSound.SyncToObject 5
    Return
m2:
    PushConstant0
    Return
