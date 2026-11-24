"""Validate the bootstrap before any USB write, without accessing hardware."""
import runpy
import sys
import tempfile
import types
from pathlib import Path
from unittest.mock import patch

usb = types.ModuleType("usb")
usb.__path__ = []
usb.core = types.ModuleType("usb.core")
usb.util = types.ModuleType("usb.util")
with patch.dict(sys.modules, {"usb": usb, "usb.core": usb.core, "usb.util": usb.util}):
    wake = runpy.run_path(str(Path(__file__).resolve().parents[1] / "scripts/wake_tuner.py"))


class Device:
    def __init__(self):
        self.calls = []

    def ctrl_transfer(self, **kwargs):
        self.calls.append(kwargs)


device = Device()
with tempfile.TemporaryDirectory(prefix="hibiki-fx2-tests-") as directory:
    corrupt = Path(directory) / "bad.bin"
    corrupt.write_bytes(b"x" * 4212)
    try:
        wake["load_firmware"](device, corrupt)
    except ValueError:
        pass
    else:
        raise AssertionError("Corrupt firmware was accepted")
    assert not device.calls, "USB writes occurred before firmware validation"

if len(sys.argv) > 1:
    wake["load_firmware"](device, sys.argv[1])
    assert len(device.calls) == 4
    assert [c["wValue"] for c in device.calls] == [0xE600, 0, 4096, 0xE600]
    assert device.calls[0]["data_or_wLength"] == [1]
    assert device.calls[-1]["data_or_wLength"] == [0]
    assert len(device.calls[1]["data_or_wLength"]) == 4096
    assert len(device.calls[2]["data_or_wLength"]) == 116
print("FX2 firmware verification and USB write ordering passed.")
