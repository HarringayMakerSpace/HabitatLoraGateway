/*  HAB Gateway
 *  
 *  *** unfinished, work in progress ***
 *  
 *  An ESP8266 WiFi module with an RFM98 LORA transceiver gateway that receives 
 *  GPS coordinates broadcast from an in flight balloon and publishes them to 
 *  the HAB HUB Tracker, http://tracker.habhub.org/.
 *  
 *  It uses the ESP8266/Arduino Over-The-Air support so the code can be updated wirelessly
 *  and tzapu's WifiManager to simplify the Wifi configuration. It runs a small web app to
 *  view the gateway status and configuration. Using an ESP module with builtin USB power 
 *  such as a NodeMCU or Wemos Mini enables powering the gateway from a USB sockect on a 
 *  PC or phone charger or chase car. So the whole gateway is small self contained and 
 *  costs less than £10 so you can have a bunch of them.    
 *  
 *  Wiring connections:
 *    ESP8266  - RFM98
 *    GND        GND
 *    VCC        VCC
 *    GPIO15     NSS
 *    GPIO13     MOSI
 *    GPIO12     MIS0
 *    GPIO14     SCK
 *    GPIO5      DIO0
 *  
 * Ant Elder
 * License: Apache License v2
*/
#include <ESP8266HTTPClient.h>
#include <time.h>
#include <SPI.h>
#include <RH_RF95.h>

// these for WifiManager https://github.com/tzapu/WiFiManager 
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          

// these for OTA upport (OTA needs ESP module with large enough flash)
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h> 

String  GATEWAY_ID = "HABGateway-"; // will have the ESP Chip ID appended

RH_RF95 rf95(15, 5); // NSS=GPIO-15, DIO0=GPIO-5

ESP8266WebServer webServer(80);

time_t startupTime;

int txReceived, txError; 

const int LOG_SIZE = 10;
int nextLogIndex;
struct LogEntry {
  time_t t;
  int rssi;
  int freqErr;
  String msg;
};
LogEntry msgLog[LOG_SIZE];

double frequency = 434.0000;
byte bandwidth = 64;
byte spreadingFactor = 11;
byte codingRate = 8;
boolean explicitHeaders = false;
boolean rateOptimization = true;

boolean configUpdated = false;

void setup() {
  Serial.begin(115200); Serial.println();

  GATEWAY_ID += String(ESP.getChipId(), HEX);
  Serial.print(GATEWAY_ID); Serial.println(", compiled: "  __DATE__  ", " __TIME__ );

  initWifiManager();
  initOTA();
  configTime(1 * 3600, 0, "pool.ntp.org", "time.nist.gov"); 
  initWebServer();
  initRF95();

  waitForNTP();
  Serial.println("Gateway running...");
}

void loop() {
  if (rf95.available()) {
    receiveTransmission();
  }
  webServer.handleClient();
  ArduinoOTA.handle();
}

void receiveTransmission() {

  uint8_t buf[255];
  uint8_t len = sizeof(buf);
  if ( ! rf95.recv(buf, &len)) {
      Serial.println("***RF95 receive error");
      txError++;
      return;
  }
      
  txReceived++;
  buf[len] = 0; // null delimit the string
      
  LogEntry le;
  le.t = time(NULL);
  le.rssi = rf95.lastRssi();
  le.freqErr = (frequencyError());
  le.msg = String((char*)buf); // Its binary data, String just a convenient container (TODO?)

   msgLog[nextLogIndex++] = le;
   if (nextLogIndex >= LOG_SIZE) nextLogIndex = 0;
            
   Serial.print("Got msg (len="); Serial.print(len); Serial.print("):"); Serial.println(le.msg);

   sendToHabitat(le);
}

void sendToHabitat(LogEntry le) {

   // TODO: how to send to Habitat?
   // Example Raspberry Pi Habitat code here: https://github.com/PiInTheSky/lora-gateway/blob/master/habitat.c 
   // looks like just an HTTP PUT with a correctly formatted payload
   /*

   HTTPClient http;
   http.begin("http://habitat.habhub.org/habitat/_design/payload_telemetry/_update/add_listener/" + doc_id);

   http.addHeader("Content-Type", "application/json");
   http.addHeader("Accept", "application/json");
   http.addHeader("charsets", "utf-8");

   String payload = String("{ \"someJson\": {");
   payload += "\"foo\": \" bla \",";
   payload += "} }"; 

   int httpCode = http.PUT(payload);
   Serial.print("HTTP POST Response: "); Serial.println(httpCode);

   http.end();
    
    */ 
   
   // for now just post some data to Thingspeak so can see something online

   String sendUrl = String("http://api.thingspeak.com/update?api_key=NG2Z4N1P6BVQSGE6&field1=") + le.msg.toInt() + "&field2=" + le.rssi;
   Serial.print("Sending to ");  Serial.println(sendUrl);
   HTTPClient http;
   http.begin(sendUrl);
   int httpCode = http.GET();
   Serial.print("HTTP GET Response: "); Serial.println(httpCode); 
   http.end();
}

void initWebServer() {
  webServer.on("/", []() {
    webServer.send(200, "text/html", getHtmlPage());
  });
  webServer.on("/setconfig", []() {
    updateRadioConfig();
  });
  webServer.begin();
  Serial.println("WebServer started on port 80");
}

String getHtmlPage() {
  // TODO store all these literals in flash?
  String response = 
    "<!DOCTYPE HTML>"
    "<HTML><HEAD>"
      "<TITLE>" + GATEWAY_ID + "</TITLE>"
    "</HEAD>"
    "<BODY>"
    "<h1>" + GATEWAY_ID + "</h1>";

  time_t t = time(NULL);
  response += "Current time is: <b>" + String(ctime(&t)) + "</b>"; 
  response += ", up since: <b>" + String(ctime(&startupTime)) + "</b>"; 
  response +="<br>";

  response +="WiFi is "; 
  if (WiFi.status() == WL_CONNECTED) {
    response+="connected to: <b>"; response += WiFi.SSID();
    response += "</b>, IP address: <b>"; response += WiFi.localIP().toString();
    response += "</b>, WiFi connection RSSI: <b>"; response += WiFi.RSSI();
    response += "</b>";
  } else {
    response+="DISCONNECTED";
  }
  response +="<br>";

  response +="Messages received: <b>"; response += txReceived; 
  response +="</b>, Receive Errors: <b>"; response += txError; 
  response +="</b>, LORA background noise RSSI: <b>"; response += (rf95.spiRead(RH_RF95_REG_1B_RSSI_VALUE) - 137); 
  response +="</b>"; 
  response +="<br>";

  response +=
    "<h2>Receiver Settings</h2>"
    "<form action=\"setconfig\">"
      "Frequency (MHz):"
      "<input type=\"text\" name=\"frequency\" value=\"" + String(frequency, 4) + "\">"
      "&nbsp;&nbsp;"
      "Spreading Factor:"
      "<select name=\"sf\">"
        "<option " + (spreadingFactor==6? "selected" : "") + " value=\"6\">6</option>"
        "<option " + (spreadingFactor==7? "selected" : "") + " value=\"7\">7</option>"
        "<option " + (spreadingFactor==8? "selected" : "") + " value=\"8\">8</option>"
        "<option " + (spreadingFactor==9? "selected" : "") + " value=\"9\">9</option>"
        "<option " + (spreadingFactor==10? "selected" : "") + " value=\"10\">10</option>"
        "<option " + (spreadingFactor==11? "selected" : "") + " value=\"11\">11</option>"
        "<option " + (spreadingFactor==12? "selected" : "") + " value=\"12\">12</option>"
      "</select>"
      "&nbsp;&nbsp;"
      "Bandwidth:"
      "<select name=\"bw\">"
        "<option " + (bandwidth==0x00? "selected" : "") + " value=\"7k8\">7k8</option>"
        "<option " + (bandwidth==0x10? "selected" : "") + " value=\"10k4\">10k4</option>"
        "<option " + (bandwidth==0x20? "selected" : "") + " value=\"15k6\">15k6</option>"
        "<option " + (bandwidth==0x30? "selected" : "") + " value=\"20k8\">20k8</option>"
        "<option " + (bandwidth==0x40? "selected" : "") + " value=\"31k25\">31k25</option>"
        "<option " + (bandwidth==0x50? "selected" : "") + " value=\"41k7\">41k7</option>"
        "<option " + (bandwidth==0x60? "selected" : "") + " value=\"62k5\">62k5</option>"
        "<option " + (bandwidth==0x70? "selected" : "") + " value=\"125k\">125k</option>"
        "<option " + (bandwidth==0x80? "selected" : "") + " value=\"250k\">250k</option>"
        "<option " + (bandwidth==0x90? "selected" : "") + " value=\"500k\">500k</option>"
      "</select>"
      "&nbsp;&nbsp;"
      "Coding rate:"
      "<select name=\"codingRate\">"
        "<option " + (codingRate==0x02? "selected" : "") + " value=\"4/5\">4/5</option>"
        "<option " + (codingRate==0x04? "selected" : "") + " value=\"4/6\">4/6</option>"
        "<option " + (codingRate==0x06? "selected" : "") + " value=\"4/7\">4/7</option>"
        "<option " + (codingRate==0x08? "selected" : "") + " value=\"4/8\">4/8</option>"
      "</select>"
      "<br>"
      "<input type=\"submit\" value=\"Update\">"
      + (configUpdated ? "<b>Updated</b>" : "") + 
    "</form> ";
  
  response +="<h2>Message Log</h2>";
  response +="<table style=\"max_width: 100%; min-width: 40%; border: 1px solid black; border-collapse: collapse;\" class=\"config_table\">";
  response +="<tr>";
  response +="<th style=\"background-color: green; color: white;\">Time</th>";
  response +="<th style=\"background-color: green; color: white;\">RSSI</th>";
  response +="<th style=\"background-color: green; color: white;\">Freq Err</th>";
  response +="<th style=\"background-color: green; color: white;\">Sentance</th>";
  response +="</tr>";
  int j = nextLogIndex;
  for (int i=0; i<LOG_SIZE; i++) {
     j--;
     if (j<0) j=LOG_SIZE-1;
     if (msgLog[j].t != NULL) {
       response +="<tr>"; 
       response +="<td style=\"border: 1px solid black;\">" + String(ctime(&msgLog[j].t)) + "</td>"; 
       response +="<td style=\"border: 1px solid black;\">" + String(msgLog[j].rssi) + "</td>"; 
       response +="<td style=\"border: 1px solid black;\">" + String(msgLog[j].freqErr) + "</td>"; 
       response +="<td style=\"border: 1px solid black;\">" + msgLog[j].msg + "</td>"; 
       response+="</tr>";
     }
  }
  response +="</table>";
    
  response +="</BODY></HTML>";

  configUpdated = false;
  
  return response;
}

void updateRadioConfig() {
    double fx = webServer.arg("frequency").toFloat();
    if (fx != frequency) {
      frequency = fx;
      rf95.setFrequency(frequency);
      configUpdated = true;
    }

   int sfx = webServer.arg("sf").toInt();
   String bws = webServer.arg("bw");
   byte bwx = bandwidthTobyte(bws);

   String crs = webServer.arg("codingRate");
   byte crx = codingRateToByte(crs);
   
   if (bwx != bandwidth || sfx != spreadingFactor || crx != codingRate) {
      bandwidth = bwx;
      spreadingFactor = sfx;
      codingRate = crx;
      rf95Config(bandwidth, spreadingFactor, codingRate, explicitHeaders, rateOptimization); 
      configUpdated = true;
   }

   // redirect back to main page
   webServer.sendHeader("Location", String("/"), true);
   webServer.send ( 302, "text/plain", "");
}

/* Configure the LORA radio settings
 *  Bandwidth can be: (See section 4.1.1.4. Signal Bandwidth)
 *  Spreading Factor can be 6 to 12 (See section 4.1.1.2. Spreading Factor)
 *  Coding Rate can be 2, 4, 6, or 8 (See section 4.1.1.3. Coding Rate)
 *  Rate Optimization can be 
 */
void rf95Config(byte bandwidth, byte spreadingFactor, byte codeRate, boolean explicitHeaders, boolean rateOptimisation) {
  RH_RF95::ModemConfig rf95Config;
  rf95Config.reg_1d = bandwidth + codeRate + (explicitHeaders ? 1 : 0);
  rf95Config.reg_1e = (spreadingFactor * 16) + 7;
  rf95Config.reg_26 = (rateOptimisation ? 0x08 : 0x00);
  
  rf95.setModemRegisters(&rf95Config);
}

void initRF95() {
  if ( ! rf95.init()) {
    Serial.println("rf95 init failed, check wiring. Restarting ...");
    ESP.restart();
  }

  rf95.setFrequency(frequency);
  rf95Config(bandwidth, spreadingFactor, codingRate, explicitHeaders, rateOptimization); 

  Serial.print("RF95 started on frequency "); Serial.print(frequency, 4);
  Serial.print("(MHz), spreading factor "); Serial.print(spreadingFactor);
  Serial.print(", bandwidth "); Serial.print(bandwidthToString(bandwidth));
  Serial.print(", coding rate "); Serial.print(codingRateToString(codingRate));
  Serial.print(", headers are "); Serial.print((explicitHeaders? "explicit" : "implicit"));
  Serial.print(", rate optimization "); Serial.print(rateOptimization);
  Serial.println();
}

void initWifiManager() {
  WiFiManager wifiManager;
  wifiManager.setTimeout(180);
  if( ! wifiManager.autoConnect(GATEWAY_ID.c_str())) {
    Serial.println("Timeout connecting. Restarting...");
    delay(1000);
    ESP.reset();
  } 

  Serial.print("WiFi connected to "); Serial.print(WiFi.SSID());
  Serial.print(", IP address: "); Serial.println(WiFi.localIP());
}

void initOTA() {
  ArduinoOTA.setHostname(GATEWAY_ID.c_str());

  ArduinoOTA.onStart([]() {
    Serial.println("OTA Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA End");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  Serial.println("OTA Ready and waiting for update...");
}

// NTP takes a few seconds to initialize with time from internet
void waitForNTP() {
   int timeout = 300; // 30 seconds
   while (time(NULL) == 0 && (timeout-- > 0)) {
     delay(100); 
   }
   startupTime = time(NULL);
   Serial.print("Time at startup: "); Serial.println(ctime(&startupTime));
}

#define REG_FREQ_ERROR 0x28

// frequency error calculation from https://github.com/daveake/LoRaArduinoSerial/blob/master/LoRaArduinoSerial.ino
double frequencyError(void) {

  int32_t Temp = (int32_t)rf95.spiRead(REG_FREQ_ERROR) & 7;
  Temp <<= 8L;
  Temp += (int32_t)rf95.spiRead(REG_FREQ_ERROR+1);
  Temp <<= 8L;
  Temp += (int32_t)rf95.spiRead(REG_FREQ_ERROR+2);
  
  if (rf95.spiRead(REG_FREQ_ERROR) & 8) {
    Temp = Temp - 524288;
  }

  double T = (double)Temp;
  T *=  (16777216.0 / 32000000.0);
  T *= (bandwidthToDecimal(bandwidth) / 500000.0);

  return -T;
} 

// TODO: get these bandwidth helpers added to RadioHead library

#define BANDWIDTH_7K8               0x00
#define BANDWIDTH_10K4              0x10
#define BANDWIDTH_15K6              0x20
#define BANDWIDTH_20K8              0x30
#define BANDWIDTH_31K25             0x40
#define BANDWIDTH_41K7              0x50
#define BANDWIDTH_62K5              0x60
#define BANDWIDTH_125K              0x70
#define BANDWIDTH_250K              0x80
#define BANDWIDTH_500K              0x90

#define BANDWIDTH_7K8_S             "7k8"
#define BANDWIDTH_10K4_S            "10k4"
#define BANDWIDTH_15K6_S            "15k6"
#define BANDWIDTH_20K8_S            "20k8"
#define BANDWIDTH_31K25_S           "31k25"
#define BANDWIDTH_41K7_S            "41k7"
#define BANDWIDTH_62K5_S            "62k5"
#define BANDWIDTH_125K_S            "125k"
#define BANDWIDTH_250K_S            "250k"
#define BANDWIDTH_500K_S            "500k"

double bandwidthToDecimal(byte bw) {
  switch (bw) {
    case  BANDWIDTH_7K8:  return 7800;
    case  BANDWIDTH_10K4:   return 10400; 
    case  BANDWIDTH_15K6:   return 15600; 
    case  BANDWIDTH_20K8:   return 20800; 
    case  BANDWIDTH_31K25:  return 31250; 
    case  BANDWIDTH_41K7:   return 41700; 
    case  BANDWIDTH_62K5:   return 62500; 
    case  BANDWIDTH_125K:   return 125000; 
    case  BANDWIDTH_250K:   return 250000; 
    case  BANDWIDTH_500K:   return 500000; 
  }
}

String bandwidthToString(byte bw) {
  switch (bw) {
    case  BANDWIDTH_7K8:    return BANDWIDTH_7K8_S;
    case  BANDWIDTH_10K4:   return BANDWIDTH_7K8_S; 
    case  BANDWIDTH_15K6:   return BANDWIDTH_7K8_S; 
    case  BANDWIDTH_20K8:   return BANDWIDTH_7K8_S; 
    case  BANDWIDTH_31K25:  return BANDWIDTH_31K25_S; 
    case  BANDWIDTH_41K7:   return BANDWIDTH_41K7_S; 
    case  BANDWIDTH_62K5:   return BANDWIDTH_62K5_S; 
    case  BANDWIDTH_125K:   return BANDWIDTH_125K_S; 
    case  BANDWIDTH_250K:   return BANDWIDTH_250K_S; 
    case  BANDWIDTH_500K:   return BANDWIDTH_500K_S; 
  }
}

byte bandwidthTobyte(String bws) {
   if (bws == BANDWIDTH_7K8_S)    return BANDWIDTH_7K8;
   if (bws == BANDWIDTH_10K4_S)   return BANDWIDTH_10K4;
   if (bws == BANDWIDTH_15K6_S)   return BANDWIDTH_15K6;
   if (bws == BANDWIDTH_20K8_S)   return BANDWIDTH_20K8;
   if (bws == BANDWIDTH_31K25_S)  return BANDWIDTH_31K25;
   if (bws == BANDWIDTH_41K7_S)   return BANDWIDTH_41K7;
   if (bws == BANDWIDTH_62K5_S)   return BANDWIDTH_62K5;
   if (bws == BANDWIDTH_125K_S)   return BANDWIDTH_125K;
   if (bws == BANDWIDTH_250K_S)   return BANDWIDTH_250K;
   if (bws == BANDWIDTH_500K_S)   return BANDWIDTH_500K;
}

#define CODING_RATE_4_5              0x02
#define CODING_RATE_4_6              0x04
#define CODING_RATE_4_7              0x06
#define CODING_RATE_4_8              0x08

#define CODING_RATE_4_5_S            "4/5"
#define CODING_RATE_4_6_S            "4/6"
#define CODING_RATE_4_7_S            "4/7"
#define CODING_RATE_4_8_S            "4/8"

String codingRateToString(byte cr) {
  switch (cr) {
    case  CODING_RATE_4_5:    return CODING_RATE_4_5_S;
    case  CODING_RATE_4_6:    return CODING_RATE_4_6_S;
    case  CODING_RATE_4_7:    return CODING_RATE_4_7_S;
    case  CODING_RATE_4_8:    return CODING_RATE_4_8_S;
  }
}

byte codingRateToByte(String crs) {
   if (crs == CODING_RATE_4_5_S)   return CODING_RATE_4_5;
   if (crs == CODING_RATE_4_6_S)   return CODING_RATE_4_6;
   if (crs == CODING_RATE_4_7_S)   return CODING_RATE_4_7;
   if (crs == CODING_RATE_4_8_S)   return CODING_RATE_4_8;
}


