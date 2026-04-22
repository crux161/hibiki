# Success!

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
