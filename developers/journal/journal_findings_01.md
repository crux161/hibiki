# Success!

linux amd64:
```
(venv) crux@cruxbook:~/hibiki$ doas ./wake_tuner.py 
[+] Found unprogrammed Cypress device at Bus 1 Address 2
[*] Holding 8051 CPU in reset...
[*] Writing 4212 bytes to device RAM...
[*] Releasing 8051 CPU reset to execute firmware...
[+] Success! The device should now disconnect and re-enumerate with a new VID/PID.
(venv) crux@cruxbook:~/hibiki$ lsusb
Bus 001 Device 001: ID 1d6b:0002 Linux Foundation 2.0 root hub
Bus 001 Device 002: ID 1f4d:e691 G-Tek Electronics Group EyeTV Stick```

macOS amd64:
│(venv) ➜  hibiki git:(main) ✗ ./wake_tuner.py                                                       │
│[+] Found unprogrammed Cypress device at Bus 20 Address 1                                           │
│[*] Holding 8051 CPU in reset...                                                                    │
│[*] Writing 4212 bytes to device RAM...                                                             │
│[*] Releasing 8051 CPU reset to execute firmware...                                                 │
│[+] Success! The device should now disconnect and re-enumerate with a new VID/PID.                  │
│(venv) ➜  hibiki git:(main) ✗ ioreg -p IOUSB -w0                                                    │
│                                                                                                    │
│+-o Root  <class IORegistryEntry, id 0x100000100, retain 24>                                        │
│  +-o AppleUSBVHCIBCE Root Hub Simulation@80000000  <class AppleUSBRootHubDevice, id 0x10000052c, re│
│gistered, matched, active, busy 0 (1 ms), retain 18>                                                │
│  | +-o Touch Bar Backlight@80700000  <class AppleUSBDevice, id 0x10000052e, registered, matched, ac│
│tive, busy 0 (0 ms), retain 11>                                                                     │
│  | +-o FaceTime HD Camera (Built-in)@80200000  <class AppleUSBDevice, id 0x100000534, registered, m│
│atched, active, busy 0 (59 ms), retain 13>                                                          │
│  | +-o Apple Internal Keyboard / Trackpad@80500000  <class AppleUSBDevice, id 0x100000539, register│
│ed, matched, active, busy 0 (2 ms), retain 19>                                                      │
│  | +-o Touch Bar Display@80600000  <class AppleUSBDevice, id 0x10000053d, registered, matched, acti│
│ve, busy 0 (1 ms), retain 13>                                                                       │
│  | +-o Headset@80400000  <class AppleUSBDevice, id 0x100000541, registered, matched, active, busy 0│
│ (1 ms), retain 11>                                                                                 │
│  | +-o Ambient Light Sensor@80300000  <class AppleUSBDevice, id 0x100000545, registered, matched, a│
│ctive, busy 0 (1 ms), retain 11>                                                                    │
│  | +-o Apple T2 Controller@80100000  <class AppleUSBDevice, id 0x100000549, registered, matched, ac│
│tive, busy 0 (0 ms), retain 13>                                                                     │
│  +-o AppleUSBXHCI Root Hub Simulation@14000000  <class AppleUSBRootHubDevice, id 0x100000c85, regis│
│tered, matched, active, busy 0 (1 ms), retain 12>                                                   │
│    +-o EyeTV Stick@14400000  <class AppleUSBDevice, id 0x100000c89, registered, matched, active, bu│
│sy 0 (1 ms), retain 12>
