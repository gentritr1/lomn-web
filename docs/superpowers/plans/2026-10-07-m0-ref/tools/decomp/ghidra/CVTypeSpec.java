// Shared helper: resolve a cvtypes2.py type spec (prim:/ptr:/struct:/arr:/enum) to a Ghidra DataType.
import ghidra.program.model.data.*;
import java.util.*;
public class CVTypeSpec {
  final DataTypeManager dtm; final Map<String, Structure> structs;
  CVTypeSpec(DataTypeManager dtm, Map<String, Structure> structs) { this.dtm = dtm; this.structs = structs; }
  DataType prim(String n) {
    switch (n) {
      case "void": return VoidDataType.dataType;
      case "char": return CharDataType.dataType; case "uchar": return UnsignedCharDataType.dataType;
      case "short": return ShortDataType.dataType; case "ushort": return UnsignedShortDataType.dataType;
      case "long": case "int": return IntegerDataType.dataType; case "ulong": case "uint": return UnsignedIntegerDataType.dataType;
      case "longlong": return LongLongDataType.dataType; case "ulonglong": return UnsignedLongLongDataType.dataType;
      case "float": return FloatDataType.dataType; case "double": return DoubleDataType.dataType;
      case "bool": return BooleanDataType.dataType; case "wchar": return WideChar16DataType.dataType;
      default: return Undefined4DataType.dataType;
    }
  }
  /** size is the CodeView size; a result whose length disagrees falls back to raw bytes so layouts never shift. */
  DataType resolve(String t, int size) {
    DataType dt = parse(t, size);
    if (dt == null || (size > 0 && dt.getLength() != size)) return size > 0 ? new ArrayDataType(Undefined1DataType.dataType, size, 1, dtm) : null;
    return dt;
  }
  DataType parse(String t, int size) {
    if (t.startsWith("ptr:")) {
      DataType target = parse(t.substring(4), -1);
      return new PointerDataType(target == null || target.getLength() == 0 && !(target instanceof VoidDataType) ? VoidDataType.dataType : target, 4, dtm);
    }
    if (t.startsWith("prim:")) return prim(t.substring(5));
    if (t.equals("enum")) return UnsignedIntegerDataType.dataType;
    if (t.startsWith("struct:")) return structs.get(t.substring(7));
    if (t.startsWith("arr:")) {
      int c = t.lastIndexOf(':'); int cnt = Integer.parseInt(t.substring(c + 1));
      if (cnt <= 0 || size <= 0 || size % cnt != 0) return null;
      DataType el = resolve(t.substring(4, c), size / cnt);
      return el == null ? null : new ArrayDataType(el, cnt, size / cnt, dtm);
    }
    return null;
  }
}
