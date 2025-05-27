#include <mem.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <OSCMessage.h>
#include <OSCBundle.h>
#include <EEPROM.h>

#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>         //https://github.com/tzapu/WiFiManager

#include <AccelStepper.h>

// Define EEPROM settings
#define EEPROM_SIZE 512
#define NAME_MAX_LENGTH 32
#define NAME_START_ADDR 0
#define NAME_VALID_FLAG_ADDR (NAME_START_ADDR + NAME_MAX_LENGTH)
#define NAME_VALID_FLAG 0xAB

char DeviceName[NAME_MAX_LENGTH] = ""; // Default name, will be overwritten

const char* HELP = "/help, /calibrate, /stats, /position 100, /speed 100, /acc 1000, /capture, /continuous_move, /stop, /set_name yourname";// (Heartbeat every 5s to 239.1.1.1:7777)";

///Low    Low   Low   Full step
///High   Low   Low   Half step
///Low    High  Low   Quarter step
///High   High  Low   Eighth step
///High   High  High  Sixteenth step

WiFiServer server(80);
char SerialContent[30];


// a instance of UDP  to send and receive packets (udp, oviously!;)
WiFiUDP Udp;
IPAddress outIp(6, 6, 6, 6);    //ip address of the receiving host
//IPAddress outIp(192,168,1,26);Incomming
const unsigned int outPort = 9120;              //host port
const unsigned int localPort = 9999;        // local port to listen for UDP packets (here's where we send the packets)

// Multicast configuration
IPAddress multicastIP(239, 1, 2, 1);  // Multicast group address
const unsigned int multicastPort = 7777;  // Multicast port
unsigned long lastHeartbeatTime = 0;
const unsigned long heartbeatInterval = 5000;  // Send heartbeat every 5 seconds


AccelStepper stepper(1, D5, D0); // mode, step, dir

const byte endStop = D4;
const int EnablePin = D7;
const int ledPin =  BUILTIN_LED;      // the number of the LED pin

unsigned long ACC = 100;
unsigned int SPEED = 1000;
unsigned long POS;

boolean constantMode = false;

boolean stopped = true;

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
  char newName[NAME_MAX_LENGTH] = {0};
  msg.getString(0, newName, NAME_MAX_LENGTH);
  
  if (strlen(newName) > 0) {
    Serial.print("Setting new name: ");
    Serial.println(newName);
    
    strncpy(DeviceName, newName, NAME_MAX_LENGTH - 1);
    DeviceName[NAME_MAX_LENGTH - 1] = 0; // Ensure null termination
    
    saveNameToEEPROM();
    
    // Update MDNS
    MDNS.end();
    if (!MDNS.begin(DeviceName)) {
      Serial.println("MDNS restart failed");
    } else {
      Serial.println("MDNS restarted with new name");
    }
    
    sendMessage("Name updated and saved");
  } else {
    sendMessage("Error: Name cannot be empty");
  }
}

void sendHeartbeat() {
  OSCMessage msg("/heartbeat");
  msg.add(DeviceName);
  msg.add((int32_t)stepper.currentPosition());

  // msg.add((int32_t)(stepper.isRunning() ? 1 : 0));

// bool is running


  bool isRunning = stepper.distanceToGo() != 0;
  msg.add((int32_t)isRunning);



  // also add speed and acc
  msg.add((int32_t)SPEED);
  msg.add((int32_t)ACC);
  
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


  WiFiManager wifiManager;

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
// join multicast group
  // Udp.beginMulticast(WiFi.localIP(), multicastIP, localPort);


  // // Start TCP (HTTP) server
  // server.begin();
  // MDNS.addService("http", "tcp", 80);
  // // Serial.print("Setup Done ");


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
  msg.send(Udp); // send the bytes to the SLIP stream
  Udp.endPacket(); // mark the end of the OSC Packet
  msg.empty(); // free space occupied by message
  delay(1);
}

void sendMessage(const char* serialMessage) {
  OSCMessage msg("/feedback");
  msg.add(serialMessage);

  Udp.beginPacket(outIp, outPort);
  msg.send(Udp); // send the bytes to the SLIP stream
  Udp.endPacket(); // mark the end of the OSC Packet
  msg.empty(); // free space occupied by message
  delay(1);
}

int readline(int readch, char *buffer, int len)
{
  static int pos = 0;
  int rpos;

  if (readch > 0) {
    switch (readch) {
      case '\n': // Ignore new-lines
        break;
      case '\r': // Return on CR
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

void routeHelp(OSCMessage &msg, int addrOffset ) {
  char boo[25];
  msg.getString(0, boo, 25);
  Serial.println("Help : ");
  Serial.println(HELP);
  sendMessage(HELP);
}

void routeStats(OSCMessage &msg, int addrOffset ) {
  char boo[25];
  msg.getString(0, boo, 25);

  String STATSN = "Stats : Pos :" ;
  STATSN +=  POS;
  STATSN += ",ACC:" ;
  STATSN +=   ACC;
  STATSN += ",SPEED:" ;
  STATSN +=   SPEED;
  Serial.println(STATSN);
  char charBuf[50];
  STATSN.toCharArray(charBuf, 50);
  sendMessage(charBuf);
}

void routeCalibrate(OSCMessage &msg, int addrOffset ) {
  calibrate();
}



void routePos(OSCMessage &msg, int addrOffset ) {
   long posN = msg.getInt(0);
   
//  char str[msg.getDataLength(0)];
//  //  Serial.print("Just Got in : ");
//  msg.getString(0, str, 8);
//  unsigned long posN = String(str).toInt();

  Serial.println("Incomming Pos : ");
  Serial.println(posN);
  sendHeartbeat();
  stepper.moveTo(posN);

}

void dispense(OSCMessage &msg, int addrOffset){
  int posN =  stepper.currentPosition() + 400;
  stepper.moveTo(posN);
}

void routeContinuousMove(OSCMessage &msg, int addrOffset ) {
  int direction = msg.getInt(0);  // 1 for forward, -1 for backward
  int speedValue = msg.getInt(1); // Speed value
  
  Serial.print("Continuous move: Direction = ");
  Serial.print(direction);
  Serial.print(", Speed = ");
  Serial.println(speedValue);
  
  // Set to continuous movement mode
  stepper.setSpeed(speedValue*direction);
  stepper.setMaxSpeed(speedValue);
  // stepper.moveTo(direction * 1000000); // Move a very large distance in the specified direction
  
  stopped = false;
  constantMode = true;
  sendMessage("Continuous movement started");
}

void routeStop(OSCMessage &msg, int addrOffset ) {
  // Stop the stepper immediately
  stepper.stop();
  stopped = true;
  constantMode = false;
  Serial.println("Motion stopped");
  sendMessage("Stopped");
}

void routeSpeed(OSCMessage &msg, int addrOffset ) {
  int speedN = msg.getInt(0);
  Serial.println("Incomming Speed : ");
  Serial.println(speedN);
  SPEED = speedN;
  stepper.setSpeed(speedN);
  stepper.setMaxSpeed(speedN);
}

void routeAcc(OSCMessage &msg, int addrOffset ) {
  int accN = msg.getInt(0);
  Serial.println("Incomming Acceleration: ");
  Serial.println(accN);
  ACC = accN;
  stepper.setAcceleration(ACC);
}

void loop() {
  // Start timing this loop iteration
  unsigned long loopStartTime = millis();
  
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
      msgIN.route("/dispense", dispense);
      msgIN.route("/continuous_move", routeContinuousMove);
      msgIN.route("/stop", routeStop);
      msgIN.route("/speed", routeSpeed);
      msgIN.route("/acc", routeAcc);
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
  
  // // Handle web client connections - but do it non-blocking
  // WiFiClient client = server.available();
  // if (client) {
  //   // Set a timeout for client handling
  //   unsigned long clientStartTime = millis();
  //   boolean clientTimedOut = false;
    
  //   // Buffer for client data
  //   String clientData = "";
    
  //   // Wait for data but with timeout
  //   while (client.connected() && !client.available() && !clientTimedOut) {
  //     if (millis() - clientStartTime > 500) { // 500ms timeout
  //       clientTimedOut = true;
  //     }
  //     // Keep handling the motor while waiting for client data
  //     if (constantMode == false) {
  //       stepper.run();
  //     } else {
  //       stepper.runSpeed();
  //     }
  //     delay(1);
  //   }
    
  //   // Only serve the web page if we didn't timeout
  //   if (!clientTimedOut) {
  //     // Prepare the response. Start with the common header:
  //     String s = "HTTP/1.1 200 OK\r\n";
  //     s += "Content-Type: text/html\r\n\r\n";
  //     s += "<!DOCTYPE HTML>\r\n<html>\r\n";
  //     // s += "IP = ";


  //     s += "<head>";
  //     s += "<style>";
  //     s += "body {";
  //     s += "    font-family: arial, sans-serif;";

  //     s += "}";

  //     s += "table {";
  //     s += "    font-family: arial, sans-serif;";
  //     s += "    border-collapse: collapse;";
  //     s += "    width: 100%;";
  //     s += "}";

  //     s += "td, th {";
  //     s += "    border: 1px solid #dddddd;";
  //     s += "    text-align: left;";
  //     s += "    padding: 8px;";
  //     s += "}";

  //     s += "tr:nth-child(even) {";
  //     s += "    background-color: #dddddd;";
  //     s += "}";
  //     s += "</style>";
  //     s += "</head>";

  //     s += "<br>";

  //     s += "<table>";
  //     s += "  <tr>";
  //     s += "    <th></th>";
  //     s += "   <th>IP</th>";
  //     s += "   <th>PORT</th>";
  //     s += " </tr>";
  //     s += " <tr>";

  //     s += "  <td>";
  //     s += String(DeviceName);
  //     s += ".local</td>";

  //     String str = "";
  //     for (int i = 0; i < 4; i++)
  //       str += i  ? "." + String(WiFi.localIP()[i]) : String(WiFi.localIP()[i]);
  //     s += "  <td>";
  //     s += str;
  //     s += "</td>";

  //     s += "  <td>";
  //     s += localPort;
  //     s += "</td>";

  //     s += " </tr>";
  //     s += "  <tr>";

  //     s += "  <td>Connected To </td>";
  //     str = "";

  //     for (int i = 0; i < 4; i++) {
  //       str += i  ? "." + String(outIp[i]) : String(outIp[i]);
  //     }

  //     s += "  <td>";
  //     s += str;
  //     s += "</td>";

  //     s += "  <td>";
  //     s += outPort;
  //     s += "</td>";

  //     // Add multicast heartbeat information
  //     s += "  </tr>";
  //     s += "  <tr>";
  //     s += "  <td>Multicast Heartbeat</td>";
  //     str = "";
  //     for (int i = 0; i < 4; i++) {
  //       str += i  ? "." + String(multicastIP[i]) : String(multicastIP[i]);
  //     }
  //     s += "  <td>";
  //     s += str;
  //     s += "</td>";
  //     s += "  <td>";
  //     s += multicastPort;
  //     s += "</td>";
  //     s += "  </tr>";
      
  //     // Add heartbeat content information
  //     s += "  <tr>";
  //     s += "  <td colspan='3'><b>Heartbeat Data</b>: Device name, Position, Running status, Speed, Acceleration</td>";
  //     s += "  </tr>";
      
  //     // Add UDP buffer information
  //     s += "  <tr>";
  //     s += "  <td colspan='3'><b>UDP Buffer Size</b>: ";
  //     s += UDP_BUFFER_SIZE;
  //     s += " bytes</td>";
  //     s += "  </tr>";
      
  //     s += "</table>";


  //     s += "<br>";
  //     s += "<br>";
  //     s += HELP;
  //     s += "<br>";
  //     s += "<br>";

  //     String mStr = "";
  //     mStr += "<table>";
  //     mStr += "  <tr>";
  //     mStr += "    <th>MS1</th>";
  //     mStr += "   <th>MS2</th>";
  //     mStr += "   <th>MS3</th>";
  //     mStr += "   <th>Resolution</th>";
  //     mStr += " </tr>";
  //     mStr += " <tr>";
  //     mStr += "  <td>0</td>";
  //     mStr += "  <td>0</td>";
  //     mStr += "  <td>0</td>";
  //     mStr += "  <td>FULL</td>";
  //     mStr += " </tr>";
  //     mStr += "  <tr>";
  //     mStr += "  <td>1</td>";
  //     mStr += "  <td>0</td>";
  //     mStr += "  <td>0</td>";
  //     mStr += "  <td>HALF</td>";
  //     mStr += "  </tr>";
  //     mStr += "  <tr>";
  //     mStr += "  <td>0</td>";
  //     mStr += "  <td>1</td>";
  //     mStr += "  <td>0</td>";
  //     mStr += "  <td>QUARTER</td>";
  //     mStr += "  </tr>";
  //     mStr += "<tr>";
  //     mStr += "  <td>1</td>";
  //     mStr += "  <td>1</td>";
  //     mStr += "  <td>0</td>";
  //     mStr += "  <td>EIGHTH</td>";
  //     mStr += "  </tr>";
  //     mStr += " <tr>";
  //     mStr += "  <td>1</td>";
  //     mStr += "  <td>1</td>";
  //     mStr += "  <td>1</td>";
  //     mStr += "  <td>SIXTEENTH</td>";
  //     mStr += "  </tr>";
  //     mStr += "</table>";

  //     s += mStr;

  //     // Add performance metrics to the web interface
  //     s += "  <tr>";
  //     s += "  <td colspan='3'><b>Last Message Processing Time</b>: ";
  //     s += packetProcessingTime;
  //     s += " ms, Max: ";
  //     s += maxProcessingTime;
  //     s += " ms</td>";
  //     s += "  </tr>";
      
  //     s += "</html>\n";

  //     client.print(s);
  //   }
    
  //   // Close the connection
  //   client.stop();
  // }
  
  // Give WiFi and system tasks a chance to run
  yield();
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
