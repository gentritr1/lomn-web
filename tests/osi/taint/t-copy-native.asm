; t-copy-native: a copy-kind stub returns a copy of argument 0: its tag plus the argument's
.function main 0
    CallGameFunction t.src 0
    CallGameFunction t.cp 1
b_copy:
    CompareAndBranchIfFalse @a
a:
    PushConstanti8 1
    CallGameFunction t.cp 1
b_copy_clean_arg:
    CompareAndBranchIfFalse @b
b:
    PushConstanti8 1
    Return
