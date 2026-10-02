READ ME - WARNING

In case of uploading firmware to ESP - only connect USB C input when the 12V / barrel jack plug is DISCONNECTED and/or main switch is OFF.
Other wise you'll have a conflicting 5V from USB and a 5V from the buck converter to the ESP controller.

Use the Arduino IDE to open the .ino file. The starup_image.h contains the startup image converted to a RGB565 format.

The Waveshare ESP appears as serial port <still to check name> 