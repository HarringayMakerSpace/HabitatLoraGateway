# Habitat LORA Gateway

An ESP8266 based Gateway for sending LORA transmissions to the Habitat high altitude balloon tracking system, http://tracker.habhub.org/.

It uses an ESP8266/Arduino WiFi microcontroller with an RFM98 LORA radio transceiver.
- ESP8266/Arduino Over-The-Air support is enabled so the gateway code can be updated wirelessly where ever the gateway is physically installed.
- tzapu's WifiManager is used to simplify the WiFi configuration.
- It runs a web server with simple web application to view/update the gateway status and radio configuration

Using an ESP module with builtin USB power such as a NodeMCU or Wemos Mini enables powering the gateway from a USB socket on a PC or phone charger or chase car which makes the whole gateway is simple, self contained, and costs less than £10 so you can have a bunch of them.   

The Gateway web application looks like this:

![Alt text](/doc/ScreenShot.jpg?raw=true "Gateway Web Page")

A gateway with minimal hardware:

![Alt text](/doc/BareBones1.jpg?raw=true "Minimal hardware")

Soldering wires directly to the LORA module is a bit tricky and fragile, a much nicer solution is [this shield](https://github.com/hallard/WeMos-Lora) for a Wemos ESP8266 board. That makes a really compact and robust gateway that just needs a USB power supply:

![Alt text](/doc/WemosRFM98.jpg?raw=true "Wemos based Gateway")

## What you need - the hardware

- An ESP8266. There are lots of different type of ESP8266 modules, this code should run on any of them. To keep it simple one of the ones with built in power supply and USB serial support are easiest, such as the [NodeMCU](https://en.wikipedia.org/wiki/NodeMCU) or [Wemos D1 Mini](https://www.wemos.cc/product/d1-mini.html). You should be able to pick one up for just a few dollars.

- A LORA radio transceiver module. They're all based on the Semtech SX127X chipset so this code should run with any of them (so far tested with the 433MHz [RFM98W](http://www.hoperf.co.uk/shop/RFM98W-433S2-RFM98W_433S2.html) and [DRF1278F](http://www.dorji.com/products-detail.php?ProId=14)).

- An antenna, which could be as simple as just 173mm long piece of wire.

- A Micro USB cable

- Connect the LORA module to the ESP8266. The LORA modules are tiny and its quite hard to solder hookup wires to them. Easier is to solder the LORA board on to a breakout board or shield. The [white adapter plates for ESP-12 modules](https://www.google.co.uk/search?q=esp12+white+adapter+plate) happen to also fit the RFM98W modules.  

Wiring connections:   

ESP8266  | LORA Board   
--- | --- |
  GND    |    GND   
  VCC    |    VCC   
  GPIO15 |    NSS   
  GPIO13 |    MOSI  
  GPIO12 |    MIS0   
  GPIO14 |    SCK   
  GPIO5  |    DIO0   
     
TODO: more detail on the connections btw the ESP and LORA board, and a wiring diagram.

## The software

You need the Arduino IDE, the ESP8266 support added to the IDE, and this sketch here to program the ESP8266 with.

- Get the Arduino IDE [here](https://www.arduino.cc/en/Main/Software).

- The Arduino IDE doesn't support the ESP8266 be default so you need to add that. Start up the IDE, go to File -> Preferences, and in the "Additional Boards Manager URLs" field add the ESP8266 URL "http://arduino.esp8266.com/stable/package_esp8266com_index.json". Then go to Tools -> Board: -> Boards Manager... By Type change "All" to "Contributed". The eSP8266 should appear, select it and then click Install. Thats it. Read more about it [here](https://github.com/esp8266/Arduino/#installing-with-boards-manager).

