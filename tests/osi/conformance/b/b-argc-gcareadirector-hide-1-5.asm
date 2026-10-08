; b-argc-gcareadirector-hide-1-5
.function main2 0 @m2
.function main 0
    PushConstantColor8888 0x11223344
err:
    CallGameFunction GcAreaDirector.Hide 1
    Return
m2:
    PushConstant0
    Return
