# HAB Gateway

A gateway for receiving telemetry transmissions from a HAB flight and publishing them to the HAB HUB Habitat Tracker, http://tracker.habhub.org/.

It uses an ESP8266/Arduino WiFi microcontroller with an RFM98 LORA radio transceiver.
- ESP8266/Arduino Over-The-Air support is enabled so the gateway code can be updated wirelessly where ever the gateway is physically installed.
- tzapu's WifiManager is used to simplify the WiFi configuration.
- It runs a web server with simple web application to view/update the gateway status and radio configuration

Using an ESP module with builtin USB power such as a NodeMCU or Wemos Mini enables powering the gateway from a USB socket on a PC or phone charger or chase car which makes the whole gateway is simple, self contained, and costs less than £10 so you can have a bunch of them.   

