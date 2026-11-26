#!/usr/bin/env python3
"""Inventory eb1a:5902 using descriptors and one read-only bridge request.

Requires PyUSB/libusb only when run against hardware. No interface claims,
configuration changes, I2C transactions, firmware uploads, or stream reads.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import sys

VID, PID = 0xEB1A, 0x5902
FIRMWARE_SIZE = 53940
FIRMWARE_SHA256 = "ee2fc156069cd7c2f0ec5a8a381bd6ab91ed5e82f6018d254f06cdae48d35872"
REPO = Path(__file__).resolve().parents[1]
# Factual register/ID definitions from the pinned Linux v6.12 em28xx driver.
REGISTER_SOURCE = "https://github.com/torvalds/linux/blob/v6.12/drivers/media/usb/em28xx/em28xx-reg.h"
REQUEST_SOURCE = "https://github.com/torvalds/linux/blob/v6.12/drivers/media/usb/em28xx/em28xx-core.c"
CHIP_MODELS = {65: "EM2874", 68: "EM2884", 113: "EM28174", 114: "EM28178"}
TRANSFER_TYPES = ("control", "isochronous", "bulk", "interrupt")


def firmware_info(path):
    path = Path(path)
    try:
        display_path = str(path.resolve().relative_to(REPO))
    except ValueError:
        display_path = str(path)
    result = {"path": display_path, "expected_size_bytes": FIRMWARE_SIZE,
              "expected_sha256": FIRMWARE_SHA256, "reference_match": False}
    try:
        digest = hashlib.sha256()
        size = 0
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(65536), b""):
                size += len(chunk)
                digest.update(chunk)
        result.update(size_bytes=size, sha256=digest.hexdigest())
        result["reference_match"] = size == FIRMWARE_SIZE and digest.hexdigest() == FIRMWARE_SHA256
    except OSError as error:
        result["error"] = str(error)
    return result


def read_bridge_chip(device):
    if (device.idVendor, device.idProduct) != (VID, PID):
        raise ValueError("Bridge register reads are restricted to eb1a:5902")
    # em28xx_read_reg: vendor/device IN, request 0, value 0, register in index.
    data = bytes(device.ctrl_transfer(bmRequestType=0xC0, bRequest=0,
                                     wValue=0, wIndex=0x0A,
                                     data_or_wLength=1, timeout=1000))
    if len(data) != 1:
        raise ValueError(f"Chip-ID register returned {len(data)} bytes; expected 1")
    return {"status": "read", "register": "0x0a", "value": data[0],
            "hex": f"0x{data[0]:02x}", "model": CHIP_MODELS.get(data[0]),
            "register_source": REGISTER_SOURCE, "request_source": REQUEST_SOURCE}


def inspect_device(device, get_string, *, descriptors_only=False):
    if (device.idVendor, device.idProduct) != (VID, PID):
        raise ValueError("Device inventory is restricted to eb1a:5902")
    result = {"vid": f"0x{device.idVendor:04x}", "pid": f"0x{device.idProduct:04x}",
              "bus": device.bus, "address": device.address,
              "port_numbers": list(getattr(device, "port_numbers", None) or []),
              "usb_version_bcd": f"0x{device.bcdUSB:04x}",
              "device_version_bcd": f"0x{device.bcdDevice:04x}",
              "speed_code": getattr(device, "speed", None),
              "configurations": [], "stream_candidates": [], "errors": []}
    for name, index in (("manufacturer", device.iManufacturer), ("product", device.iProduct)):
        try:
            result[name] = get_string(device, index) if index else None
        except (OSError, ValueError) as error:
            result[name] = None
            result["errors"].append(f"{name}: {error}")
    try:
        for config in device:
            conf = {"value": config.bConfigurationValue, "attributes": config.bmAttributes,
                    "max_power_raw": config.bMaxPower, "interfaces": []}
            result["configurations"].append(conf)
            for interface in config:
                alt = {"number": interface.bInterfaceNumber,
                       "alternate_setting": interface.bAlternateSetting,
                       "class": interface.bInterfaceClass,
                       "subclass": interface.bInterfaceSubClass,
                       "protocol": interface.bInterfaceProtocol, "endpoints": []}
                conf["interfaces"].append(alt)
                for ep in interface:
                    kind = TRANSFER_TYPES[ep.bmAttributes & 3]
                    incoming = bool(ep.bEndpointAddress & 0x80)
                    endpoint = {"address": f"0x{ep.bEndpointAddress:02x}",
                                "direction": "in" if incoming else "out",
                                "transfer_type": kind, "attributes": ep.bmAttributes,
                                "max_packet_size_raw": ep.wMaxPacketSize,
                                "interval": ep.bInterval}
                    alt["endpoints"].append(endpoint)
                    if incoming and kind in ("bulk", "isochronous"):
                        result["stream_candidates"].append({
                            "configuration": config.bConfigurationValue,
                            "interface": interface.bInterfaceNumber,
                            "alternate_setting": interface.bAlternateSetting,
                            "endpoint": endpoint["address"], "transfer_type": kind,
                            "role": "unverified"})
    except (OSError, ValueError) as error:
        result["errors"].append(f"descriptors: {error}")
    if descriptors_only:
        result["bridge_chip_id"] = {"status": "not_requested"}
    else:
        try:
            result["bridge_chip_id"] = read_bridge_chip(device)
        except (OSError, ValueError) as error:
            result["bridge_chip_id"] = {"status": "error", "error": str(error)}
            result["errors"].append(f"bridge chip ID: {error}")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bus", type=int, help="restrict to this USB bus")
    parser.add_argument("--address", type=int, help="restrict to this USB device address")
    parser.add_argument("--descriptors-only", action="store_true", help="skip the vendor-IN chip-ID read")
    parser.add_argument("--firmware", type=Path, default=REPO / "fw/c306_main.ram.bin")
    parser.add_argument("--output", type=Path, help="write JSON here instead of stdout")
    args = parser.parse_args(argv)
    report = {"schema_version": 1, "captured_at_utc": datetime.now(timezone.utc).isoformat(),
              "target": "eb1a:5902", "read_only": True,
              "descriptors_only": args.descriptors_only,
              "host": {"system": platform.system(), "machine": platform.machine()},
              "firmware": firmware_info(args.firmware), "devices": [], "errors": []}
    code = 0
    try:
        # Deliberately lazy: pure tests and --help do not require PyUSB.
        import usb.core
        import usb.util
    except ImportError:
        report["errors"].append("PyUSB is unavailable; install pyusb and libusb or use the repository venv")
        code = 3
    else:
        try:
            for device in usb.core.find(find_all=True, idVendor=VID, idProduct=PID):
                if args.bus is not None and device.bus != args.bus:
                    continue
                if args.address is not None and device.address != args.address:
                    continue
                try:
                    report["devices"].append(inspect_device(
                        device, usb.util.get_string, descriptors_only=args.descriptors_only))
                finally:
                    usb.util.dispose_resources(device)
        except (OSError, ValueError) as error:
            report["errors"].append(f"USB inventory: {error}")
            code = 1
        if not report["devices"] and not report["errors"]:
            report["errors"].append("No matching eb1a:5902 device at the requested bus/address")
            code = 2
    if code == 0 and (not report["firmware"]["reference_match"] or
                      any(device["errors"] for device in report["devices"])):
        code = 1
    payload = json.dumps(report, indent=2) + "\n"
    if args.output:
        try:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(payload)
        except OSError as error:
            print(f"Cannot save probe report: {error}", file=sys.stderr)
            return 1
        print(f"Saved {len(report['devices'])} device report(s) to {args.output}", file=sys.stderr)
    else:
        sys.stdout.write(payload)
    return code


if __name__ == "__main__":
    sys.exit(main())
