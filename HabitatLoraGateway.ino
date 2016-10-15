/*  Habitat LORA Gateway
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
#include <EEPROM.h>
#include <time.h>
#include <base64.h>
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

// Change NSS and DIO-0 pins to match how your modules are wired together
//#define NSS_PIN 15
//#define DIO0_PIN 5
#define NSS_PIN 16
#define DIO0_PIN 15

RH_RF95 rf95(NSS_PIN, DIO0_PIN);

ESP8266WebServer webServer(80);

time_t startupTime;

int txReceived, txError; 

struct LogEntry {
  time_t t;
  int rssi;
  int freqErr;
  int habitatRC;
  String msg;
};
int nextLogIndex;
const int LOG_SIZE = 10;
LogEntry msgLog[LOG_SIZE];

// these will retained over power off by being persisted in EEPROM
double frequency = 434.0000;
byte bandwidth = 64;
byte spreadingFactor = 11;
byte codingRate = 8;
boolean implicitHeaders = true;
boolean rateOptimization = true;
boolean afc = true;
boolean habitat = false;
String  gatewayName = "HMS-"; // default name will have the ESP Chip ID appended

boolean configUpdated = false;
boolean afcChange = false;

void setup() {
  Serial.begin(115200); Serial.println();
  Serial.println("Habitat LORA Gateway, compiled: "  __DATE__  ", " __TIME__ );

  gatewayName += String(ESP.getChipId(), HEX);
  loadConfig();
  printConfig();
  
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

  LogEntry le;
  le.t = time(NULL);
  le.rssi = rf95.lastRssi();
  le.freqErr = frequencyError();
  le.habitatRC = 0;

  // the RadioHead library uses the first four bytes as headers. HAB
  // transmissions don't use headers so just use the header bytes as the payload

  if (rf95.headerTo() == '$' && rf95.headerFrom() == '$') {
    buf[len] = 0x00; // ensure null terminated
    le.msg = String((char)rf95.headerTo()) + (char) rf95.headerFrom() + (char) rf95.headerId() + (char) rf95.headerFlags();
    le.msg += String((char*)buf);     
  } else {
    le.msg = String(rf95.headerTo(), HEX) + String(rf95.headerFrom(), HEX) +
            " " + String(rf95.headerId(), HEX) + String(rf95.headerFlags(), HEX) + " ";
    le.msg = byteArrayToHexString(buf, len);
  }

  if (habitat && le.msg.startsWith("$$")) {
    le.habitatRC = sendToHabitat(le);
  }

  msgLog[nextLogIndex++] = le;
  if (nextLogIndex >= LOG_SIZE) nextLogIndex = 0;
            
  Serial.println("LogEntry " + getRFC3339Time(le.t) + ", RSSI=" + le.rssi + ", Freq Err=" + le.freqErr + ", payload=" + le.msg);

  if (afc) { 
    doAFC();
  }
}

// $$test1,1,01:23:45,51.58680343,-0.10234091,23*28\n
int sendToHabitat(LogEntry le) {

   // TODO: do this async in the background so as not to block LORA receives? 

  String sentence = le.msg.endsWith("\n") ? le.msg : (le.msg + "\n");
  String b64Sentence = base64::encode(sentence);
  String sha256Sentence = sha256Hash(b64Sentence);

   HTTPClient http;
   http.begin("http://habitat.habhub.org/habitat/_design/payload_telemetry/_update/add_listener/" + sha256Sentence);

   http.addHeader("Content-Type", "application/json");
   http.addHeader("Accept", "application/json");
   http.addHeader("charsets", "utf-8");

   String timeNow = getRFC3339Time(time(NULL));
   
   String payload = "{" 
    "\"data\": {"
        "\"_raw\": \"" + b64Sentence + "\""
    "},"
    "\"receivers\": {"
       "\"" + gatewayName + "\": {"
            "\"time_created\": \"" + timeNow + "\","
            "\"time_uploaded\": \"" + timeNow + "\""
         "}"
       "}"
    "}";

   Serial.print("Habitat payload: "); Serial.println(payload);
   
   int httpCode = http.sendRequest("PUT", payload);
   Serial.print("Habitat PUT Response: "); Serial.println(httpCode);
   
   http.end();
   return httpCode;
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
      "<TITLE>" + gatewayName + "</TITLE>"
    "</HEAD>"
    "<BODY>"
    "<h1>Habitat LORA Gateway: " + gatewayName + "</h1>";

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
  response +="<br><br>";

  response +="Messages received: <b>"; response += txReceived; 
  response +="</b>, Receive Errors: <b>"; response += txError; 
  response +="</b>, LORA background noise RSSI: <b>"; response += (rf95.spiRead(RH_RF95_REG_1B_RSSI_VALUE) - 137); 
  response +="</b>"; 
  response +="<br>";

  response +=
    "<h2>Settings</h2>"
    "<form action=\"setconfig\">"
      "Gateway Name:"
      "<input type=\"text\" name=\"gatewayName\" value=\"" + gatewayName + "\">"
      "<br>"
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

      "<input type=\"checkbox\" name=\"implicitHeaders\" value=\"On\" " + (implicitHeaders ? "checked" : "") + ">Implicit Headers" 
      "&nbsp;&nbsp;&nbsp;"

      "<input type=\"checkbox\" name=\"rateOptimization\" value=\"On\" " + (rateOptimization ? "checked" : "") + ">Rate Optimization" 
      "&nbsp;&nbsp;&nbsp;"

      "<input type=\"checkbox\" name=\"afc\" value=\"On\" " + (afc ? "checked" : "") + ">AFC" 
      "&nbsp;&nbsp;&nbsp;"

      "<input type=\"checkbox\" name=\"habitat\" value=\"On\" " + (habitat ? "checked" : "") + ">Habitat<br>" 

      "<input type=\"submit\" value=\"Update\">"
      
      + (configUpdated ? "<b>Updated</b>" : "") + 
      + (afcChange ? "&nbsp;<b>**AFC Change**</b>" : "") + 
    "</form> ";
  
  response +="<h2>Message Log</h2>";
  response +="<table style=\"max_width: 100%; min-width: 40%; border: 1px solid black; border-collapse: collapse;\" class=\"config_table\">";
  response +="<colgroup><col span=\"1\" style=\"width: 36%;\"><col span=\"1\" style=\"width: 8%;\"><col span=\"1\" style=\"width: 8%;\"><col span=\"1\" style=\"width: 8%;\"><col span=\"1\" style=\"width: 40%;\"></colgroup>";
  response +="<tr>";
  response +="<th style=\"background-color: green; color: white;\">Time</th>";
  response +="<th style=\"background-color: green; color: white;\">RSSI</th>";
  response +="<th style=\"background-color: green; color: white;\">Freq Err</th>";
  response +="<th style=\"background-color: green; color: white;\">Habitat</th>";
  response +="<th style=\"background-color: green; color: white;\">Sentence</th>";
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
       response +="<td style=\"border: 1px solid black;\">" + (msgLog[j].habitatRC != 0 ? String(msgLog[j].habitatRC) : "") + "</td>"; 
       response +="<td style=\"border: 1px solid black;\">" + msgLog[j].msg + "</td>"; 
       response+="</tr>";
     }
  }
  response +="</table>";
    
  response +="</BODY></HTML>";

  configUpdated = false;
  afcChange = false;
  
  return response;
}

void updateRadioConfig() {
    double fx = webServer.arg("frequency").toFloat();
    if (fx != frequency) {
      frequency = fx;
      rf95.setModeIdle();
      rf95.setFrequency(frequency);
      rf95.setModeRx();
      configUpdated = true;
    }

   int sfx = webServer.arg("sf").toInt();

   String bws = webServer.arg("bw");
   byte bwx = bandwidthTobyte(bws);

   String crs = webServer.arg("codingRate");
   byte crx = codingRateToByte(crs);

   String implicitHeadersS = webServer.arg("implicitHeaders");
   boolean implicitHeadersx = (implicitHeadersS == "On");

   String rateOptimizationS = webServer.arg("rateOptimization");
   boolean rateOptimizationx = (rateOptimizationS == "On");

   if (bwx != bandwidth || sfx != spreadingFactor || crx != codingRate || implicitHeadersx != implicitHeaders || rateOptimizationx != rateOptimization) {
      bandwidth = bwx;
      spreadingFactor = sfx;
      codingRate = crx;
      implicitHeaders = implicitHeadersx;
      rateOptimization = rateOptimizationx;
      rf95Config(bandwidth, spreadingFactor, codingRate, implicitHeaders, rateOptimization); 
      configUpdated = true;
   }

   String afcs = webServer.arg("afc");
   boolean afcx = (afcs == "On");
   if (afcx != afc) {
    afc = afcx;
    configUpdated = true;
   }

   String habitats = webServer.arg("habitat");
   boolean habitatx = (habitats == "On");
   if (habitatx != habitat) {
    habitat = habitatx;
    configUpdated = true;
   }

   String gns = webServer.arg("gatewayName");
   if (gns != gatewayName) {
     gatewayName = gns;
     configUpdated = true;
   }
   
   if (configUpdated) {
      persistConfig();
   }
   
   // redirect back to main page
   webServer.sendHeader("Location", String("/"), true);
   webServer.send ( 302, "text/plain", "");
}

#define EEPROM_SAVED_MARKER 72

void persistConfig() {
  EEPROM.begin(512);
  EEPROM.write(0, EEPROM_SAVED_MARKER); // flag to indicate EEPROM contains a config
  int addr = 1;
  EEPROM.put(addr, frequency);          addr += sizeof(frequency);
  EEPROM.put(addr, spreadingFactor);    addr += sizeof(spreadingFactor);
  EEPROM.put(addr, bandwidth);          addr += sizeof(bandwidth);
  EEPROM.put(addr, codingRate);         addr += sizeof(codingRate);
  EEPROM.put(addr, implicitHeaders);    addr += sizeof(implicitHeaders);
  EEPROM.put(addr, rateOptimization);   addr += sizeof(rateOptimization);
  EEPROM.put(addr, afc);                addr += sizeof(afc); 
  addr = eepromWriteString(addr, gatewayName);
  EEPROM.put(addr, habitat);            addr += sizeof(habitat); 

  // update loadConfig() and printConfig() if anything else added here
  
  EEPROM.commit();

  Serial.print("Saved "); printConfig();
}

void loadConfig() {
  EEPROM.begin(512);

  if (EEPROM.read(0) != EEPROM_SAVED_MARKER) {
    Serial.println("Using default config");
    return; 
  }

  int addr = 1;
  EEPROM.get(addr, frequency);               addr += sizeof(frequency);
  EEPROM.get(addr, spreadingFactor);         addr += sizeof(spreadingFactor);
  EEPROM.get(addr, bandwidth);               addr += sizeof(bandwidth);
  EEPROM.get(addr, codingRate);              addr += sizeof(codingRate);
  EEPROM.get(addr, implicitHeaders);         addr += sizeof(implicitHeaders);
  EEPROM.get(addr, rateOptimization);        addr += sizeof(rateOptimization);
  EEPROM.get(addr, afc);                     addr += sizeof(afc); 
  gatewayName = eepromReadString(addr);      addr += gatewayName.length() + 1;
  EEPROM.get(addr, habitat);                 addr += sizeof(habitat); 
}

void printConfig() {
  Serial.print("Gateway Name '"); Serial.print(gatewayName); 
  Serial.print("', Frequency="); Serial.print(frequency, 4); 
  Serial.print(" (MHz), SpreadingFactor="); Serial.print(spreadingFactor);
  Serial.print(", Bandwidth="); Serial.print(bandwidthToString(bandwidth));
  Serial.print(", ECC codingRate="); Serial.print(codingRateToString(codingRate));
  Serial.print(", Headers are "); Serial.print(implicitHeaders? "Implicit" : "Explicit");
  Serial.print(", Rate Optimization is "); Serial.print(rateOptimization? "On" : "Off");
  Serial.print(", AFC is "); Serial.print(afc? "On" : "Off");
  Serial.print(", Habitat is "); Serial.print(habitat? "On" : "Off");
  Serial.println();
}

// are these eeprom read/write String functions in Arduino code somewhere?
int eepromWriteString(int addr, String s) {
  int l = s.length();
  for (int i=0; i<l; i++) {
     EEPROM.write(addr++, s.charAt(i));
  }
  EEPROM.write(addr++, 0x00);
  return addr;  
}

String eepromReadString(int addr) {
  String s;
  char c;
  while ((c = EEPROM.read(addr++)) != 0x00) {
     s += c;
  }
  return s;
}

/* Configure the LORA radio settings
 *  Bandwidth can be: (See section 4.1.1.4. Signal Bandwidth)
 *  Spreading Factor can be 6 to 12 (See section 4.1.1.2. Spreading Factor)
 *  Coding Rate can be 2, 4, 6, or 8 (See section 4.1.1.3. Coding Rate)
 *  Rate Optimization can be on or off and should be on with SF 11 and 12 
 *  
 *  The best description of the settings is in the SX1278 datasheet 
 */
void rf95Config(byte bandwidth, byte spreadingFactor, byte codingRate, boolean implicitHeaders, boolean rateOptimisation) {
  RH_RF95::ModemConfig rf95Config;

  rf95Config.reg_1d = implicitHeaders | codingRate | bandwidth;
  rf95Config.reg_1e = (spreadingFactor * 16) | 0x04; // 0x04 sets CRC on
  rf95Config.reg_26 = 0x04 | (rateOptimisation ? 0x08 : 0x00); // 0x04 sets AGC on, rateOptimisation should be on for SF 11  and 12

  Serial.print("rf95 config registers 0x1d:"); Serial.print(rf95Config.reg_1d, HEX);
  Serial.print(", 0x1e:"); Serial.print(rf95Config.reg_1e, HEX);
  Serial.print(", 0x26:"); Serial.println(rf95Config.reg_26, HEX);

  rf95.setModeIdle();
  rf95.setModemRegisters(&rf95Config);
  rf95.setModeRx();
}

void initRF95() {
  if ( ! rf95.init()) {
    Serial.println("rf95 init failed, check wiring. Restarting ...");
    ESP.restart();
  }

  rf95.setPromiscuous(true); // don't want the RadioHead header addressing
  rf95.setFrequency(frequency);
  rf95.spiWrite(RH_RF95_REG_0C_LNA, 0x23); // LNA Max Gain
  rf95Config(bandwidth, spreadingFactor, codingRate, implicitHeaders, rateOptimization); 
}

void initWifiManager() {
  WiFiManager wifiManager;
  wifiManager.setTimeout(180);
  if( ! wifiManager.autoConnect(gatewayName.c_str())) {
    Serial.println("Timeout connecting. Restarting...");
    delay(1000);
    ESP.reset();
  } 

  Serial.print("WiFi connected to "); Serial.print(WiFi.SSID());
  Serial.print(", IP address: "); Serial.println(WiFi.localIP());
}

void initOTA() {
  ArduinoOTA.setHostname(gatewayName.c_str());

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

/* The frequency of the LORA transmissions drifts over time (due to temperature changes?)
 *  this tries to compensate by adjusting the Gateway receiver frequency based on the 
 *  frequency error of the received messages. 
 */
void doAFC() {
  int i = nextLogIndex - 1;
  if (i < 0) i = LOG_SIZE - 1;
  int fe = msgLog[i].freqErr;  

  if (abs(fe) > 100) {
    Serial.print("*** AFC: adjusting frequency by "); Serial.print(fe); Serial.println(" Hz ***");
    frequency += (fe / 1000000.0);
    rf95.setModeIdle();
    rf95.setFrequency(frequency);
    rf95.setModeRx();
    persistConfig();    
    afcChange = true;
  }  
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

// returns the XOR checksum of a String
String xorChecksum(String s) {
  byte b = s.charAt(0);
  for (int i=1; i<s.length(); i++) {
    b = b ^ s.charAt(i);
  }
  String checksum = String(b, HEX);
  if (checksum.length() ==1) checksum = "0" + checksum; 
  return checksum;
}

// Turns a ctime string "Ddd Mmm DD HH:MM:SS YYYY" into HH:MM:SS
String getTimeNow(time_t t) {
  String ts = String(ctime(&t));
  return ts.substring(11, 19);
}

/* Turns a ctime string "Ddd Mmm DD HH:MM:SS YYYY" into
 * an RFC3339 format string "YYYY-MM-DDTHH:MM:SS+00:00" 
 * (a bit hacky)
 */
String getRFC3339Time(time_t t) {
  String ts = String(ctime(&t));
  String rfc3339 = ts.substring(ts.length()-5, ts.length()-1);
  rfc3339 += '-';
  int monthInt;
  String monthS = ts.substring(4,7);
  if (monthS.equals("Jan")) monthInt = 1;
  else if (monthS.equals("Feb")) monthInt = 2;
  else if (monthS.equals("Mar")) monthInt = 3;
  else if (monthS.equals("Apr")) monthInt = 4;
  else if (monthS.equals("May")) monthInt = 5;
  else if (monthS.equals("Jun")) monthInt = 6;
  else if (monthS.equals("Jul")) monthInt = 7;
  else if (monthS.equals("Aug")) monthInt = 8;
  else if (monthS.equals("Sep")) monthInt = 9;
  else if (monthS.equals("Oct")) monthInt = 10;
  else if (monthS.equals("Nov")) monthInt = 11;
  else monthInt = 12;
  rfc3339 += monthInt;
  rfc3339 += '-';
  rfc3339 += ts.substring(8,10);
  rfc3339 += 'T';
  rfc3339 += ts.substring(11,19);
  rfc3339 += "+00:00";
    
  return rfc3339;  
}

// Bytes to hex string, in pairs with uppercase and leading zeros 
String byteArrayToHexString(uint8_t buf[], uint8_t len) {
  String s;
  for (int i=0; i<len; i++) {
    if (i>0 && (i%2 == 0)) s+= ' ';     
    String c = String(buf[i], HEX);
    if (c.length() < 2) {
      s+= '0'; // add leading zero
    }
    s+= c;
  }
  s.toUpperCase();
  return s;
}

#define REG_FREQ_ERROR 0x28

// frequency error calculation from https://github.com/daveake/LoRaArduinoSerial/blob/master/LoRaArduinoSerial.ino
double frequencyError() {

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
    case  BANDWIDTH_7K8:    return 7800;
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
    case  BANDWIDTH_10K4:   return BANDWIDTH_10K4_S; 
    case  BANDWIDTH_15K6:   return BANDWIDTH_15K6_S; 
    case  BANDWIDTH_20K8:   return BANDWIDTH_20K8_S; 
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

