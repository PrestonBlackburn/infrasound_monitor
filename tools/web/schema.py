# The C data structures for the web app
from dataclasses import make_dataclass
from pathlib import Path
from typing import Iterator, NamedTuple

from cffi import FFI

ROOT = Path(__file__).resolve().parents[2]
STATE_HEADER = ROOT / "main" / "state" / "state_struct.h"
STATE_TYPE = "state_t"

PRELUDE = """
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef int                 int32_t;
typedef signed char         int8_t;
typedef unsigned long long  uint64_t;
typedef signed long long    int64_t;
typedef unsigned long       size_t;
typedef _Bool               bool;
"""

CANON =  {
    "unsigned char":      "uint8_t",
    "signed char":        "int8_t",
    "unsigned short":     "uint16_t",
    "short":              "int16_t",
    "unsigned int":       "uint32_t",
    "int":                "int32_t",
    "unsigned long long": "uint64_t",
    "long long":          "int64_t",
    "long":               "int64_t",
    "unsigned long":      "size_t",
    "_Bool":              "bool",
    "float":              "float",
    "double":             "double",
}

PY_TYPES = {
    "uint8_t": int,  "int8_t":  int,
    "uint16_t": int, "int16_t": int,
    "uint32_t": int, "int32_t": int,
    "uint64_t": int, "int64_t": int,
    "size_t":   int,
    "float":    float, "double": float,
}

MOCK_ONLY_KEYS = frozenset({"seq", "t", "value", "window"})

class FieldSpec(NamedTuple):
    name: str
    ctype: str
    length: int | None
    is_enum: bool

def load(header: Path = STATE_HEADER):
    """Parse C header into cffi type"""
    try:
        ffi = FFI()
        ffi.cdef(PRELUDE + header.read_text())
    except Exception as e:
        raise SystemExit(
            f"couln't parse {header}: {e}\n Should only contain typedefs, no #define or functions"
        ) from e
    return ffi, ffi.typeof(STATE_TYPE)

def fields(ffi, ct) -> Iterator[FieldSpec]:
    """map c fields to python"""
    for name, f in ct.fields:
        t = f.type
        if t.kind == "primitive":
            yield FieldSpec(name, CANON.get(t.cname, t.cname), None, False)
        elif t.kind == "array":
            yield FieldSpec(name, CANON.get(t.item.cname, t.item.cname), t.length, False)
        elif t.kind == "enum":
            yield FieldSpec(name, t.cname, None, True)
        else:
            raise SystemExit(f"state_t.{name}: unsupported cffi kind: {t.kind!r} ({t.cname})")

def _py_type(spec: FieldSpec):
    base = PY_TYPES.get(spec.ctype)
    if base is None:
        raise SystemExit(
            f"{STATE_TYPE}.{spec.name}: no python type supported for C type {spec.ctype!r}"
        )
    return base

def snapshot_class(ffi=None, ct=None, name:str = "Snapshot"):
    """dataclass mirroring state_t"""
    if ffi is None or ct is None:
        ffi, ct = load()

    attrs = []
    for spec in fields(ffi, ct):
        if spec.length is not None:
            base = _py_type(spec)
            attrs.append((spec.name, f"tuple[{base.__name__}, ...]", ()))
        elif spec.is_enum:
            attrs.append((spec.name, "int", 0))
        elif spec.ctype == "bool":
            attrs.append((spec.name, "bool", False))
        else:
            base = _py_type(spec)
            attrs.append((spec.name, base.__name__, 0 if base is int else 0.0))

    return make_dataclass(name, attrs)


def unknown_keys(payload: dict) -> list[str]:
    """Keys the payload carries that state_struct.h has no field for.

    This is the guard against the failure mode where a typo in a template or
    the mock surfaces as `undefined` in the browser and nowhere else.
    """
    ffi, ct = load()
    known = {spec.name for spec in fields(ffi, ct)}
    return sorted(
        k for k in payload if k not in known and k not in MOCK_ONLY_KEYS
    )



if __name__ == "__main__":
    ffi, ct = load()
    for spec in fields(ffi, ct):
        shape = f"[{spec.length}]" if spec.length else ""
        kind = "enum" if spec.is_enum else spec.ctype
        print(f"{spec.name:14s} {kind}{shape}")

    python_class = snapshot_class(ffi, ct, name="TestCStruct")
    print(python_class)
    print(python_class(
        uptime_s=123, 
        heap_free=234, 
        temperature_c = 27,
        raw_db = 67,
        raw_pressure_mpa = 0.3,
        samples = [1,2,3],
        wifi_up = True)
    )