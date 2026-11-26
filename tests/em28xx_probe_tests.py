"""Exercise device inventory and read-only USB boundaries without hardware/PyUSB."""

import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "probe_em28xx", Path(__file__).resolve().parents[1] / "scripts/probe_em28xx.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class Descriptor(types.SimpleNamespace):
    def __iter__(self):
        return iter(self.children)


def endpoint(address, attributes=2):
    return Descriptor(bEndpointAddress=address, bmAttributes=attributes,
                      wMaxPacketSize=512, bInterval=0)


class Device:
    idVendor, idProduct = 0xEB1A, 0x5902
    bus, address = 1, 7
    bcdUSB, bcdDevice = 0x200, 0x100
    iManufacturer, iProduct = 0, 2
    port_numbers, speed = (2, 4), 3

    def __init__(self, response=b"\x71"):
        self.response = response
        self.calls = []

    def __iter__(self):
        alts = [Descriptor(bInterfaceNumber=0, bAlternateSetting=alt,
                           bInterfaceClass=255, bInterfaceSubClass=0,
                           bInterfaceProtocol=0,
                           children=[endpoint(0x84), endpoint(0x85)]) for alt in (0, 1)]
        return iter([Descriptor(bConfigurationValue=1, bmAttributes=128,
                               bMaxPower=250, children=alts)])

    def ctrl_transfer(self, **request):
        # A fake controller rejects every request outside the read-only contract.
        expected = dict(bmRequestType=0xC0, bRequest=0, wValue=0,
                        wIndex=0x0A, data_or_wLength=1, timeout=1000)
        if request != expected:
            raise AssertionError(f"Unexpected USB request: {request}")
        self.calls.append(request)
        if isinstance(self.response, Exception):
            raise self.response
        return self.response

    def __getattr__(self, name):
        raise AssertionError(f"Unexpected device operation: {name}")


def get_string(device, index):
    if index != 2:
        raise AssertionError("Unexpected string descriptor request")
    return "USB ATSC Device"


class ProbeTests(unittest.TestCase):
    def test_connected_layout_and_chip_identity(self):
        dev = Device()
        report = probe.inspect_device(dev, get_string)
        self.assertFalse(report["errors"])
        self.assertEqual(report["product"], "USB ATSC Device")
        self.assertEqual(report["bridge_chip_id"]["model"], "EM28174")
        self.assertEqual(len(dev.calls), 1)
        self.assertEqual(len(report["configurations"][0]["interfaces"]), 2)
        self.assertEqual([(c["alternate_setting"], c["endpoint"]) for c in report["stream_candidates"]],
                         [(0, "0x84"), (0, "0x85"), (1, "0x84"), (1, "0x85")])
        self.assertTrue(all(c["role"] == "unverified" for c in report["stream_candidates"]))

    def test_descriptors_only_never_reads_vendor_registers(self):
        dev = Device()
        result = probe.inspect_device(dev, get_string, descriptors_only=True)
        self.assertEqual(result["bridge_chip_id"]["status"], "not_requested")
        self.assertEqual(dev.calls, [])

    def test_other_device_is_rejected_before_access(self):
        dev = Device()
        dev.idVendor, dev.idProduct = 0x1F4D, 0xE691
        for operation in (lambda: probe.inspect_device(dev, get_string),
                          lambda: probe.read_bridge_chip(dev)):
            with self.assertRaises(ValueError):
                operation()
        self.assertEqual(dev.calls, [])

    def test_unknown_chip_is_recorded_without_guessing(self):
        result = probe.inspect_device(Device(b"\xfe"), get_string)
        self.assertEqual(result["bridge_chip_id"]["value"], 254)
        self.assertIsNone(result["bridge_chip_id"]["model"])

    def test_short_reads_and_usb_errors_leave_descriptor_evidence(self):
        for response in (b"", b"\x71\x00", OSError("access denied")):
            with self.subTest(response=response):
                dev = Device(response)
                result = probe.inspect_device(dev, get_string)
                self.assertEqual(result["bridge_chip_id"]["status"], "error")
                self.assertTrue(result["errors"])
                self.assertEqual(len(result["stream_candidates"]), 4)
                self.assertEqual(len(dev.calls), 1)

    def test_only_input_stream_endpoints_are_candidates(self):
        class MixedDevice(Device):
            def __iter__(self):
                return iter([Descriptor(bConfigurationValue=2, bmAttributes=128,
                    bMaxPower=50, children=[Descriptor(bInterfaceNumber=1,
                        bAlternateSetting=3, bInterfaceClass=255,
                        bInterfaceSubClass=0, bInterfaceProtocol=0,
                        children=[endpoint(0x01), endpoint(0x82, 3), endpoint(0x86, 1)])])])
        result = probe.inspect_device(MixedDevice(), get_string)
        self.assertEqual(len(result["stream_candidates"]), 1)
        self.assertEqual(result["stream_candidates"][0]["endpoint"], "0x86")
        self.assertEqual(result["stream_candidates"][0]["transfer_type"], "isochronous")
        self.assertEqual(result["stream_candidates"][0]["configuration"], 2)

    def test_descriptor_error_is_reported_with_chip_result(self):
        class BrokenDescriptors(Device):
            def __iter__(self):
                raise OSError("configuration unavailable")
        result = probe.inspect_device(BrokenDescriptors(), get_string)
        self.assertTrue(result["errors"])
        self.assertEqual(result["configurations"], [])
        self.assertEqual(result["bridge_chip_id"]["model"], "EM28174")

    def test_firmware_missing_and_wrong_same_size(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "candidate.bin"
            self.assertIn("error", probe.firmware_info(path))
            path.write_bytes(b"x" * probe.FIRMWARE_SIZE)
            result = probe.firmware_info(path)
            self.assertEqual(result["size_bytes"], 53940)
            self.assertFalse(result["reference_match"])

    def test_firmware_reference_requires_both_size_and_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "candidate.bin"
            path.write_bytes(b"abc")
            with patch.object(probe, "FIRMWARE_SIZE", 3), patch.object(
                    probe, "FIRMWARE_SHA256",
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"):
                self.assertTrue(probe.firmware_info(path)["reference_match"])
                with patch.object(probe, "FIRMWARE_SIZE", 4):
                    self.assertFalse(probe.firmware_info(path)["reference_match"])

    def test_cli_no_device_and_partial_device_exit_codes(self):
        usb = types.ModuleType("usb")
        usb.__path__ = []
        usb.core = types.ModuleType("usb.core")
        usb.util = types.ModuleType("usb.util")
        usb.util.get_string = get_string
        disposed = []
        usb.util.dispose_resources = disposed.append
        with patch.dict("sys.modules", {"usb": usb, "usb.core": usb.core, "usb.util": usb.util}), \
             patch.object(probe, "firmware_info", return_value={"reference_match": True}), \
             contextlib.redirect_stdout(io.StringIO()):
            usb.core.find = lambda **kwargs: []
            self.assertEqual(probe.main([]), 2)
            dev = Device(OSError("device disconnected"))
            usb.core.find = lambda **kwargs: [dev]
            self.assertEqual(probe.main([]), 1)
            self.assertEqual(disposed, [dev])
            self.assertEqual(probe.main(["--bus", "99"]), 2)


if __name__ == "__main__":
    unittest.main()
