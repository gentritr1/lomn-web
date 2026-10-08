.function main2 0
    PushConstanti8 1
    Return
.class C props(a)
.function main 0
    CreateObject C
fail:
    CompareAndBranchIfFalse @else
    PushConstanti8 1
    Return
else:
    PushConstanti8 2
    Return
