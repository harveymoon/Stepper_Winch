#include <mem.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <OSCMessage.h>
#include <OSCBundle.h>
#include <EEPROM.h>

#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>  //https://github.com/tzapu/WiFiManager

#include <AccelStepper.h>

// Define EEPROM settings
#define EEPROM_SIZE 512
#define NAME_MAX_LENGTH 32
#define NAME_START_ADDR 0
#define NAME_VALID_FLAG_ADDR (NAME_START_ADDR + NAME_MAX_LENGTH)
#define NAME_VALID_FLAG 0xAB

char DeviceName[NAME_MAX_LENGTH] = "";  // Default name, will be overwritten

const char *HELP = "/help, /calibrate, /stats, /position 100, /set_current 0, /rel_move 400, /speed 100, /acc 1000, /continuous_move, /stop, /enable 1|0, /set_name yourname";

///Low    Low   Low   Full step
///High   Low   Low   Half step
///Low    High  Low   Quarter step
///High   High  Low   Eighth step
///High   High  High  Sixteenth step

WiFiServer server(80);
char SerialContent[30];


// a instance of UDP  to send and receive packets (udp, oviously!;)
WiFiUDP Udp;
IPAddress outIp(6, 6, 6, 6);  //ip address of the receiving host
//IPAddress outIp(192,168,1,26);Incomming
const unsigned int outPort = 9120;    //host port
const unsigned int localPort = 9999;  // local port to listen for UDP packets (here's where we send the packets)

// Multicast configuration
IPAddress multicastIP(239, 1, 2, 1);      // Multicast group address
const unsigned int multicastPort = 7777;  // Multicast port
unsigned long lastHeartbeatTime = 0;
const unsigned long heartbeatInterval = 5000;  // Send heartbeat every 5 seconds


AccelStepper stepper(1, D5, D0);  // mode, step, dir

const byte endStop = D4;
const int EnablePin = D7;
const int ledPin = BUILTIN_LED;  // the number of the LED pin

unsigned long ACC = 100;
unsigned int SPEED = 1000;
unsigned long POS;

boolean constantMode = false;

boolean stopped = true;

// Track whether the motor driver is currently enabled (active LOW pin logic)
bool motorEnabled = true;  // set true after setup config of EnablePin LOW

// End-stop interrupt state (non-blocking handling)
volatile bool endStopFlag = false;            // set by ISR when end stop triggers
volatile unsigned long endStopLastMicros = 0; // debounce timer
const unsigned long ENDSTOP_DEBOUNCE_US = 5000; // 5 ms debounce

ICACHE_RAM_ATTR void endStopISR() {
  unsigned long now = micros();
  if (now - endStopLastMicros > ENDSTOP_DEBOUNCE_US) {
    endStopLastMicros = now;
    endStopFlag = true; // main loop will act
  }
}

// Define a reasonable buffer size for UDP packets
// OSC messages are typically small, but we want to handle larger ones if needed
#define UDP_BUFFER_SIZE 256

// Add some timing variables to track performance
unsigned long lastPacketTime = 0;
unsigned long packetProcessingTime = 0;
unsigned long maxProcessingTime = 0;

void generateUniqueName() {
  // Use MAC address to create a unique name
  uint8_t mac[6];
  WiFi.macAddress(mac);
  
  // Format: Stepper_XXXX where XXXX is the last 2 bytes of MAC address in hex
  sprintf(DeviceName, "Stepper_%02X%02X", mac[4], mac[5]);
  
  Serial.print("Generated unique name: ");
  Serial.println(DeviceName);
}

void saveNameToEEPROM() {
  // Write the name to EEPROM
  for (int i = 0; i < NAME_MAX_LENGTH; i++) {
    EEPROM.write(NAME_START_ADDR + i, DeviceName[i]);
    // Stop at null terminator
    if (DeviceName[i] == 0) break;
  }
  
  // Write validation flag
  EEPROM.write(NAME_VALID_FLAG_ADDR, NAME_VALID_FLAG);
  
  // Commit the changes
  EEPROM.commit();
  
  Serial.print("Name saved to EEPROM: ");
  Serial.println(DeviceName);
}

bool loadNameFromEEPROM() {
  // Check if there's a valid name stored
  if (EEPROM.read(NAME_VALID_FLAG_ADDR) != NAME_VALID_FLAG) {
    Serial.println("No valid name in EEPROM");
    return false;
  }
  
  // Read the name
  for (int i = 0; i < NAME_MAX_LENGTH; i++) {
    DeviceName[i] = EEPROM.read(NAME_START_ADDR + i);
    // Stop at null terminator
    if (DeviceName[i] == 0) break;
  }
  
  Serial.print("Name loaded from EEPROM: ");
  Serial.println(DeviceName);
  return true;
}

void routeSetName(OSCMessage &msg, int addrOffset) {
  char newName[NAME_MAX_LENGTH] = { 0 };
  msg.getString(0, newName, NAME_MAX_LENGTH);
  
  if (strlen(newName) > 0) {
    Serial.print("Setting new name: ");
    Serial.println(newName);
    
    strncpy(DeviceName, newName, NAME_MAX_LENGTH - 1);
    DeviceName[NAME_MAX_LENGTH - 1] = 0;  // Ensure null termination
    
    saveNameToEEPROM();
    
    // Update MDNS
    MDNS.end();
    if (!MDNS.begin(DeviceName)) {
      Serial.println("MDNS restart failed");
    } else {
      Serial.println("MDNS restarted with new name");
    }
  // Update OTA hostname too (no need to re-begin, just set new hostname before next begin if ever restarted)
  ArduinoOTA.setHostname(DeviceName);
    
    sendMessage("Name updated and saved");
  } else {
    sendMessage("Error: Name cannot be empty");
  }
}

void sendHeartbeat() {

  // sends a heartbeat message, message includes this format:
  // /heartbeat <DeviceName> <CurrentPosition> <IsRunning> <Speed> <Acceleration> <Enabled>
  OSCMessage msg("/heartbeat");
  msg.add(DeviceName);
  msg.add((int32_t)stepper.currentPosition());
  
  msg.add((int32_t)(stepper.distanceToGo() != 0)); // is running boolean
  
  // also add speed, acc, enabled flag
  msg.add((int32_t)SPEED);
  msg.add((int32_t)ACC);
  msg.add((int32_t)(motorEnabled ? 1 : 0));
  
  Udp.beginPacket(multicastIP, multicastPort);
  msg.send(Udp);
  Udp.endPacket();
  msg.empty();
  
  // Serial.print("Heartbeat sent to ");
  // Serial.print(multicastIP);
  // Serial.print(":");
  // Serial.println(multicastPort);
}

void setup() {
  Serial.begin(115200);
  Serial.println("STARTING");
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Try to load name from EEPROM, generate a unique one if not found
  if (!loadNameFromEEPROM()) {
    generateUniqueName();
    saveNameToEEPROM();
  }
  
  pinMode(EnablePin, OUTPUT);
  
  digitalWrite(EnablePin, LOW);
  
  ESP.wdtFeed();
  
    // ==== OTA SETUP ====
  ArduinoOTA.setHostname(DeviceName); // use the dynamic device name
  // Optional: set a password (uncomment & change) or use hash
  // ArduinoOTA.setPassword("change_me");
  WiFi.mode(WIFI_STA);                   // must be STA before setting hostname
  WiFi.hostname(DeviceName);

  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(300);
  wifiManager.setHostname(DeviceName);   // <-- give WiFiManager the same hostname
  wifiManager.autoConnect(DeviceName);
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  
  // disable esp wifi sleep
  // WiFi.setSleepMode(WIFI_NONE_SLEEP);
  
  Udp.begin(localPort);
  // Udp.setTimeout(0); // make sure parsePacket() doesn't block
  Serial.println(Udp.localPort());
  ESP.wdtFeed();
  if (!MDNS.begin(DeviceName)) {
    while (1) {
      delay(1000);
    }
  }
  // Start HTTP status server
  server.begin();
  MDNS.addService("http", "tcp", 80);


  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? F("sketch") : F("filesystem");
    Serial.print(F("OTA Start updating "));
    Serial.println(type);
    // If updating FS: unmount here (SPIFFS.end()/LittleFS.end()) if used
  });
  ArduinoOTA.onEnd([]() {
    Serial.println(F("\nOTA End"));
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned int lastPct = 101; // force first print
    unsigned int pct = (progress * 100) / total;
    if (pct != lastPct) { // reduce spam
      lastPct = pct;
      Serial.printf("OTA Progress: %u%%\r", pct);
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println(F("Auth Failed"));
    else if (error == OTA_BEGIN_ERROR) Serial.println(F("Begin Failed"));
    else if (error == OTA_CONNECT_ERROR) Serial.println(F("Connect Failed"));
    else if (error == OTA_RECEIVE_ERROR) Serial.println(F("Receive Failed"));
    else if (error == OTA_END_ERROR) Serial.println(F("End Failed"));
  });
  ArduinoOTA.begin();
  Serial.println(F("OTA Ready"));
  Serial.print(F("OTA Hostname: ")); Serial.println(DeviceName);
  
  Serial.println(DeviceName);
  // date and time of compilation
  Serial.println(__DATE__);
  Serial.println(__TIME__);
  
  stepper.setMaxSpeed(1000);
  stepper.setSpeed(1000);
  stepper.setAcceleration(1000.0);
  stepper.setCurrentPosition(0);
  
  pinMode(ledPin, OUTPUT);
  pinMode(endStop, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(endStop), endStopISR, FALLING); // active LOW switch
  delay(1000);
  //calibrate();
  //delay(15000);

  // Send initial heartbeat
  sendHeartbeat();
}

void sendPos(int nowPos) {
  OSCMessage msg("/pos");
  msg.add((int32_t)nowPos);
  Udp.beginPacket(outIp, outPort);
  msg.send(Udp);    // send the bytes to the SLIP stream
  Udp.endPacket();  // mark the end of the OSC Packet
  msg.empty();      // free space occupied by message
  delay(1);
}

void sendMessage(const char *serialMessage) {
  OSCMessage msg("/feedback");
  msg.add(serialMessage);
  
  Udp.beginPacket(outIp, outPort);
  msg.send(Udp);    // send the bytes to the SLIP stream
  Udp.endPacket();  // mark the end of the OSC Packet
  msg.empty();      // free space occupied by message
  delay(1);
}

int readline(int readch, char *buffer, int len) {
  static int pos = 0;
  int rpos;
  
  if (readch > 0) {
    switch (readch) {
      case '\n':  // Ignore new-lines
      break;
      case '\r':  // Return on CR
      rpos = pos;
      pos = 0;  // Reset position index ready for next time
      return rpos;
      default:
      if (pos < len - 1) {
        buffer[pos++] = readch;
        buffer[pos] = 0;
      }
    }
  }
  // No end of line has been found, so return -1.
  return -1;
}

void routeHelp(OSCMessage &msg, int addrOffset) {
  char boo[25];
  msg.getString(0, boo, 25);
  Serial.println("Help : ");
  Serial.println(HELP);
  sendMessage(HELP);
}

void routeStats(OSCMessage &msg, int addrOffset) {
  char boo[25];
  msg.getString(0, boo, 25);
  
  String STATSN = "Stats : Pos :";
  STATSN += POS;
  STATSN += ",ACC:";
  STATSN += ACC;
  STATSN += ",SPEED:";
  STATSN += SPEED;
  STATSN += ",EN:";
  STATSN += (motorEnabled ? 1 : 0);
  Serial.println(STATSN);
  char charBuf[50];
  STATSN.toCharArray(charBuf, 50);
  sendMessage(charBuf);
}

void routeCalibrate(OSCMessage &msg, int addrOffset) {
  calibrate();
}

void routePos(OSCMessage &msg, int addrOffset) {
  long posN = msg.getInt(0);
  Serial.println("Incomming Pos : ");
  Serial.println(posN);
  sendHeartbeat();
  stepper.moveTo(posN);
   stopped = false;
}

// Relative move: /rel_move <deltaSteps>
void routeRelMove(OSCMessage &msg, int addrOffset) {
  long delta = msg.getInt(0); // positive or negative
  long target = stepper.currentPosition() + delta;
  stepper.moveTo(target);
   stopped = false;
  Serial.print("Relative move: delta="); Serial.print(delta); Serial.print(" target="); Serial.println(target);
  sendMessage("Relative move queued");
  sendHeartbeat(); // reflect new target intention
}

void routeContinuousMove(OSCMessage &msg, int addrOffset) {
  int direction = msg.getInt(0);   // 1 for forward, -1 for backward
  int speedValue = msg.getInt(1);  // Speed value
  
  Serial.print("Continuous move: Direction = ");
  Serial.print(direction);
  Serial.print(", Speed = ");
  Serial.println(speedValue);
  
  // Set to continuous movement mode
  stepper.setSpeed(speedValue * direction);
  stepper.setMaxSpeed(speedValue);
  // stepper.moveTo(direction * 1000000); // Move a very large distance in the specified direction
  
  stopped = false;
  constantMode = true;
  sendMessage("Continuous movement started");
}

void routeStop(OSCMessage &msg, int addrOffset) {
  // Stop the stepper immediately
  stepper.stop();
  stopped = true;
  constantMode = false;
  Serial.println("Motion stopped");
  sendMessage("Stopped");
}

void routeSpeed(OSCMessage &msg, int addrOffset) {
  int speedN = msg.getInt(0);
  Serial.println("Incomming Speed : ");
  Serial.println(speedN);
  SPEED = speedN;
  stepper.setSpeed(speedN);
  stepper.setMaxSpeed(speedN);
}

void routeAcc(OSCMessage &msg, int addrOffset) {
  int accN = msg.getInt(0);
  Serial.println("Incomming Acceleration: ");
  Serial.println(accN);
  ACC = accN;
  stepper.setAcceleration(ACC);
}

// Set the current logical position of the stepper without moving (e.g. reset to 0)
void routeSetCurrent(OSCMessage &msg, int addrOffset) {
  long newPos = msg.getInt(0);
  stepper.setCurrentPosition(newPos);
  POS = newPos; // keep cached value aligned until next loop refresh
  Serial.print("Current position manually set to: ");
  Serial.println(newPos);
  sendMessage("Current position set");
  sendHeartbeat(); // broadcast change
}

// Enable (1) / Disable (0) the stepper driver via its Enable pin (active LOW on most drivers)
void routeEnable(OSCMessage &msg, int addrOffset) {
  int en = msg.getInt(0);  // expect 1 (enable) or 0 (disable)
  if (en == 1) {
    // Enable driver (active LOW)
    digitalWrite(EnablePin, LOW);
    motorEnabled = true;
    sendMessage("Motor Enabled");
    Serial.println("Motor Enabled via /enable 1");
  } else {
    // Stop any motion before disabling
    stepper.stop();
    constantMode = false;
    // Disable driver (active LOW so write HIGH)
    digitalWrite(EnablePin, HIGH);
    motorEnabled = false;
    sendMessage("Motor Disabled");
    Serial.println("Motor Disabled via /enable 0");
  }
}

void calibrate() {
  Serial.println("calibrating");
  sendMessage("Calibrating");
  stepper.setSpeed(1000);
  stepper.moveTo(-10000);
  
  while (digitalRead(endStop) == 1) {
    stepper.run();
    delay(10);
  }
  delay(1000);
  Serial.print("calibrated!");
  sendMessage("Calibrated!");
  stepper.setSpeed(30);
  stepper.setCurrentPosition(0);
  stepper.moveTo(100);
}


// Build a compact HTML status page (auto-refresh every 2s)
String buildStatusPage() {
  IPAddress ip = WiFi.localIP();
  String html;
  html.reserve(1400);
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'><title>");
  html += DeviceName;
  html += F(" Status</title><meta name='viewport' content='width=device-width,initial-scale=1'><meta http-equiv='refresh' content='2'>");
  html += F("<style>body{font-family:Arial;background:#111;color:#eee;margin:12px;}table{border-collapse:collapse;}td,th{border:1px solid #444;padding:4px 8px;font-size:13px;}h1{margin:0 0 8px;font-size:20px;}code{color:#9cf;}small{color:#888;} .ok{color:#6c6;} .warn{color:#fc3;} .err{color:#f66;} pre{white-space:pre-wrap;font-size:11px;background:#222;padding:8px;border:1px solid #333;} a{color:#6cf;}</style></head><body>");
  html += F("<h1>"); html += DeviceName; html += F("</h1>");
  html += F("<table>");
  html += F("<tr><th>Field</th><th>Value</th></tr>");
  html += F("<tr><td>IP</td><td>"); html += ip.toString(); html += ':'; html += localPort; html += F("</td></tr>");
  html += F("<tr><td>Remote IP</td><td>"); html += outIp.toString(); html += ':'; html += outPort; html += F("</td></tr>");
  html += F("<tr><td>Multicast</td><td>"); html += multicastIP.toString(); html += ':'; html += multicastPort; html += F("</td></tr>");
  html += F("<tr><td>Position</td><td>"); html += (long)POS; html += F("</td></tr>");
  html += F("<tr><td>Target</td><td>"); html += (long)stepper.targetPosition(); html += F("</td></tr>");
  html += F("<tr><td>DistanceToGo</td><td>"); html += (long)stepper.distanceToGo(); html += F("</td></tr>");
  html += F("<tr><td>Speed</td><td>"); html += (long)SPEED; html += F("</td></tr>");
  html += F("<tr><td>Acceleration</td><td>"); html += (long)ACC; html += F("</td></tr>");
  html += F("<tr><td>Enabled</td><td>"); html += (motorEnabled?F("Yes"):F("No")); html += F("</td></tr>");
  html += F("<tr><td>Mode</td><td>"); html += (constantMode?F("Constant Speed"):F("MoveTo")); html += F("</td></tr>");
  html += F("<tr><td>Moving</td><td>"); html += (stepper.distanceToGo()!=0?F("Yes"):F("No")); html += F("</td></tr>");
  html += F("<tr><td>Last Packet Proc (ms)</td><td>"); html += packetProcessingTime; html += F("</td></tr>");
  html += F("<tr><td>Max Packet Proc (ms)</td><td>"); html += maxProcessingTime; html += F("</td></tr>");
  html += F("<tr><td>Heartbeat Interval (ms)</td><td>"); html += heartbeatInterval; html += F("</td></tr>");
  html += F("</table>");
  html += F("<h3>OSC Commands</h3><pre>");
  html += HELP;
  html += F("</pre><small>Auto-refresh 2s. Heartbeat multicast to ");
  html += multicastIP.toString(); html += ':'; html += multicastPort; html += F(".</small></body></html>");
  return html;
}

void loop() {
  // Handle OTA first to ensure timely updates
  ArduinoOTA.handle();
  
  // Check for UDP packets first to prioritize communication
  int packetSize = Udp.parsePacket();
  if (packetSize > 0) {
    lastPacketTime = millis();
    unsigned long processStart = millis();
    
    // Create a buffer for the incoming packet
    uint8_t packetBuffer[UDP_BUFFER_SIZE];
    
    // Read the packet into the buffer
    int bytesRead = Udp.read(packetBuffer, min(packetSize, UDP_BUFFER_SIZE));
    
    // Create and process the OSC message
    OSCMessage msgIN;
    
    // Feed the entire buffer to the OSC message parser
    for (int i = 0; i < bytesRead; i++) {
      msgIN.fill(packetBuffer[i]);
    }
    
    if (!msgIN.hasError()) {
      msgIN.route("/help", routeHelp);
      msgIN.route("/stats", routeStats);
      msgIN.route("/position", routePos);
      msgIN.route("/rel_move", routeRelMove);
      msgIN.route("/continuous_move", routeContinuousMove);
      msgIN.route("/stop", routeStop);
      msgIN.route("/speed", routeSpeed);
      msgIN.route("/acc", routeAcc);
      msgIN.route("/enable", routeEnable);
      msgIN.route("/set_current", routeSetCurrent);
      msgIN.route("/calibrate", routeCalibrate);
      msgIN.route("/set_name", routeSetName);
      //      msgIN.route("/capture", routeCapture);
    } else {
      // Only print errors occasionally to avoid console spam
      static unsigned long lastErrorTime = 0;
      if (millis() - lastErrorTime > 5000) {
        Serial.println("Error parsing OSC message");
        lastErrorTime = millis();
      }
    }
    
    outIp = Udp.remoteIP();
    
    // Calculate processing time for this packet
    packetProcessingTime = millis() - processStart;
    if (packetProcessingTime > maxProcessingTime) {
      maxProcessingTime = packetProcessingTime;
    }
  }
  
  // Handle motor control - keep this as quick as possible
  if (constantMode == false) {
    stepper.run();
  } else {
    stepper.runSpeed();
  }
  POS = stepper.currentPosition();
  
  // Handle position reporting and status changes
  if (stepper.distanceToGo() == 0) {
    if (stopped == false) {
      Serial.print("* ");
      Serial.println(stepper.currentPosition());
      stopped = true;
      // sendMessage("OK");
      sendHeartbeat();
    }
  }
  
  // Send heartbeat at regular intervals - using non-blocking approach
  unsigned long currentTime = millis();
  if (currentTime - lastHeartbeatTime >= heartbeatInterval) {
    lastHeartbeatTime = currentTime;
    sendHeartbeat();
  }

  // Handle end-stop trigger (deferred from ISR to keep ISR tiny)
  if (endStopFlag) {
    noInterrupts(); // brief critical section to clear flag safely
    endStopFlag = false;
    interrupts();
    // Stop motion for safety
    stepper.stop();
    constantMode = false;
    stopped = true;
    sendMessage("EndStop Triggered");
    Serial.println("EndStop Triggered (interrupt)");
    sendHeartbeat();
  }
  
  
  WiFiClient client = server.available();
  if (client) {
    // Basic request consume (optional, minimal parse just to skip headers)
    unsigned long tStart = millis();
    while (client.connected() && client.available() == 0 && millis() - tStart < 50) yield(); // this yields and allows other tasks to run
    while (client.available()) { // discard request data quickly
      char c = client.read();
      if (c == '\n') break; // stop after first line
    }
    String body = buildStatusPage();
    client.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n"));
    client.print(body);
    delay(1);
    client.stop();
  }
  
  // if (Serial.available()) {
  //   static char buffer[80];
  //   static unsigned long lastSerialRead = 0;
  
  //   // Only process serial occasionally to avoid spending too much time here
  //   if (millis() - lastSerialRead > 100) {
  //     while (Serial.available()) {
  //       if (readline(Serial.read(), buffer, 80) > 0) {
  //         sendMessage(buffer);
  //       }
  //     }
  //     lastSerialRead = millis();
  //   }
  // }
  
  ESP.wdtFeed();
  
  yield();
}

