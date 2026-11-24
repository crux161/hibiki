#!/usr/bin/env python3

import usb.core
import usb.util
import sys
import argparse
import hashlib
from pathlib import Path

# Default Cypress EZ-USB FX2LP unprogrammed VID/PID
# 1f4d:e691 
VID = 0x1F4D
PID = 0xE691

# Cypress Vendor Request for RAM download
REQ_RAM_DOWNLOAD = 0xA0
# CPU Control & Status Register (CPUCS) address
CPUCS_REG = 0xE600

def set_cpu_reset(dev, reset_state):
    """Holds or releases the 8051 CPU from reset."""
    dev.ctrl_transfer(
        bmRequestType=0x40,  # Vendor, Device, Out
        bRequest=REQ_RAM_DOWNLOAD,
        wValue=CPUCS_REG,
        wIndex=0,
        data_or_wLength=[1 if reset_state else 0]
    )

def load_firmware(dev, filepath):
    """Writes the binary payload to the FX2LP internal RAM."""
    try:
        with open(filepath, 'rb') as f:
            firmware = f.read()
    except FileNotFoundError:
        print(f"Error: Could not find '{filepath}'.")
        sys.exit(1)

    if len(firmware) != 4212 or hashlib.sha256(firmware).hexdigest() != \
            "0c3348a7f30fba3c9d3a1463f2583ce64f6af7922da4e60531f05473ce711d2a":
        raise ValueError("FX2 bootstrap size/SHA-256 mismatch; no firmware sent")

    # 1. Hold 8051 in reset
    print("[*] Holding 8051 CPU in reset...")
    set_cpu_reset(dev, True)

    # 2. Write firmware to RAM in chunks
    chunk_size = 4096
    address = 0x0000

    print(f"[*] Writing {len(firmware)} bytes to device RAM...")
    for i in range(0, len(firmware), chunk_size):
        chunk = firmware[i:i+chunk_size]
        dev.ctrl_transfer(
            bmRequestType=0x40,
            bRequest=REQ_RAM_DOWNLOAD,
            wValue=address,
            wIndex=0,
            data_or_wLength=chunk
        )
        address += len(chunk)

    # 3. Release 8051 reset to boot the new firmware
    print("[*] Releasing 8051 CPU reset to execute firmware...")
    set_cpu_reset(dev, False)

def main():
    parser = argparse.ArgumentParser(description="Wake the FX2 bridge using verified firmware.")
    parser.add_argument("--fw", type=Path, help="path to the FX2 bootstrap binary")
    args = parser.parse_args()
    relative = Path("PT930_MN88553/PT930_pre_symbol_8051_region.bin")
    installed = Path.home() / "Library/Application Support/hibiki/fw" / relative
    firmware_path = args.fw or (installed if installed.is_file() else
                               Path(__file__).resolve().parent.parent / "fw" / relative)
    # Find the unprogrammed Cypress chip
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    
    if dev is None:
        print(f"[-] Device {hex(VID)}:{hex(PID)} not found.")
        print("    Ensure the board is plugged in with a data-capable cable.")
        sys.exit(1)

    print(f"[+] Found unprogrammed Cypress device at Bus {dev.bus} Address {dev.address}")
    
    try:
        load_firmware(dev, firmware_path)
        print("[+] Success! The device should now disconnect and re-enumerate with a new VID/PID.")
    except usb.core.USBError as e:
        print(f"[-] USB Error during transfer: {e}")
        print("    (Note: A 'No such device' error right at the end usually means success, as the chip disconnected to reboot).")
        return 1
    except Exception as e:
        print(f"[-] Unexpected error: {e}")
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
