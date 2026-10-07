"""M1a-3 Task 8B F2: the datacheck material checksum is ORDERED (material order is the attribute-id order DrawSubset
uses), not a sorted multiset. tools/research/oracle/check_datacheck.py material_sum must match the C++ lomn_datacheck
(compared on the real install by make datacheck)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "research" / "oracle"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "research"))
import check_datacheck  # noqa: E402

A = ((1.0, 0.0, 0.0, 1.0), 0.0, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), "red")
B = ((0.0, 1.0, 0.0, 1.0), 2.0, (0.5, 0.5, 0.5), (0.0, 0.0, 0.0), None)


def test_material_sum_depends_on_order():
    assert check_datacheck.material_sum([A, B]) != check_datacheck.material_sum([B, A])


def test_material_sum_known_value():
    # One material: the 11 f32 bit patterns + '|red', M0 checksum (sum = sum * 31 + byte); with no separator before it.
    line = "3f800000" + "00000000" * 2 + "3f800000" + "00000000" * 7 + "|red"
    s = 0
    for b in line.encode("latin1"):
        s = (s * 31 + b) & 0xFFFFFFFF
    assert check_datacheck.material_sum([A]) == "%08x" % s
