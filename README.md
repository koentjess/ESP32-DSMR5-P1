# ESP32-DSMR5-P1
An arduino script for reading a DSMR5 P1 sensor and sending the messages through mqtt. The Esp32 removes the need for extra components to invert the signal coming from the p1 port. This simplifies the setup significantly. 

**Disclaimer**
The code still contains a lot of junk from things that have been removed or are not working.

**Features**
- OTA updating
- Mqtt messages
- Inverted uart

**The code is mostly a combination of multiple different projecs:**
- https://klushok.etv.tudelft.nl/projects/view?id=8
- https://github.com/daniel-jong/esp8266_p1meter
- https://github.com/nldroid/CustomP1UartComponent
