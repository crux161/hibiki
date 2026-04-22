#!/usr/bin/env python3.10

import usb.core
import usb.util
import sys

# Default Cypress EZ-USB FX2LP unprogrammed VID/PID
VID = 0x04B4
PID = 0x8613

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
    # Find the unprogrammed Cypress chip
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    
    if dev is None:
        print(f"[-] Device {hex(VID)}:{hex(PID)} not found.")
        print("    Ensure the board is plugged in with a data-capable cable.")
        sys.exit(1)

    print(f"[+] Found unprogrammed Cypress device at Bus {dev.bus} Address {dev.address}")
    
    # Path to your extracted 8051 binary
    firmware_path = "fw/PT930_MN88553/PT930_pre_symbol_8051_region.bin" 
    
    try:
        load_firmware(dev, firmware_path)
        print("[+] Success! The device should now disconnect and re-enumerate with a new VID/PID.")
    except usb.core.USBError as e:
        print(f"[-] USB Error during transfer: {e}")
        print("    (Note: A 'No such device' error right at the end usually means success, as the chip disconnected to reboot).")
    except Exception as e:
        print(f"[-] Unexpected error: {e}")

if __name__ == "__main__":
    main()
