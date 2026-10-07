from x87fix import fix

EQ = ("if ((byte)((byte)((ushort)((ushort)(NAN(_DAT_0075f9cc) || NAN(this->mAimDistance)) << 10) >> 8) |\n"
      "            (byte)((ushort)((ushort)(_DAT_0075f9cc == this->mAimDistance) << 0xe) >> 8)) == 0x40) {")
LT_WHILE = ("while ((byte)(this->mAimDistance < _DAT_0075f9d4 |\n"
            "                 (byte)((ushort)((ushort)(NAN(this->mAimDistance) || NAN(_DAT_0075f9d4)) << 10) >> 8\n"
            "                       )) == 1) {")
LT_IF = ("if ((byte)(fVar6 < this->mAimDistance |\n"
         "              (byte)((ushort)((ushort)(NAN(fVar6) || NAN(this->mAimDistance)) << 10) >> 8)) == 1) {")


def test_equal_idiom():
    out, n = fix(EQ)
    assert n == 1 and out == "if ((_DAT_0075f9cc == this->mAimDistance /*x87*/)) {"


def test_less_than_idiom_with_newline_before_paren():
    out, n = fix(LT_WHILE)
    assert n == 1 and out == "while ((this->mAimDistance < _DAT_0075f9d4 /*x87*/)) {"


def test_less_than_idiom():
    out, n = fix(LT_IF)
    assert n == 1 and out == "if ((fVar6 < this->mAimDistance /*x87*/)) {"


def test_unrelated_byte_cast_untouched():
    src = "x = (byte)(a | b) == 1;"
    assert fix(src) == (src, 0)
