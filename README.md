# ESP-TCP-Transceiver
a small program for ESP32 dev boards that can send and receive short messages (128 characters) over WiFi and TCP

## How to use
- replace wifi ssid, password, and PC IP in the code and flash to ESP with idf.py
- open up netcat or your favorite TCP receiver on the PC on port 8080 and enjoy
### Binds
- Short pin 23 to GND to select a character to add to the buffer
- Short pin 22 to GND to add the selected character to the buffer
- Short pin 19 to GND to send the message


An LED on pin 21 will light up when the ESP gets an IP Address from DHCP (thus is ready to receive messages)
