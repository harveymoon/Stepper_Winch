#include <mem.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <OSCMessage.h>
#include <OSCBundle.h>

#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>         //https://github.com/tzapu/WiFiManager

#include <AccelStepper.h>

const char* DeviceName = "CarSpinner";

char* HELP = "/help, /calibrate, /stats, /position 100, /speed 100, /acc 1000, /capture";

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



AccelStepper stepper(1, D5, D0); // mode, step, dir

const byte endStop = D4;
const int EnablePin = D7;
const int ledPin =  BUILTIN_LED;      // the number of the LED pin

unsigned long ACC = 100;
unsigned int SPEED = 1000;
unsigned long POS;


boolean stopped = true;


void setup() {


  Serial.begin(115200);

  Serial.println("STARTING");

  pinMode(EnablePin, OUTPUT);

  digitalWrite(EnablePin, LOW);

  ESP.wdtFeed();


  WiFiManager wifiManager;

  wifiManager.autoConnect(DeviceName);

  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  Udp.begin(localPort);
  Serial.println(Udp.localPort());
  ESP.wdtFeed();
  if (!MDNS.begin(DeviceName)) {
    while (1) {
      delay(1000);
    }
  }

  // Start TCP (HTTP) server
  server.begin();
  MDNS.addService("http", "tcp", 80);
  // Serial.print("Setup Done ");

  Serial.println("HI HI! _ SliderApp!");

  stepper.setMaxSpeed(1000);
  stepper.setSpeed(1000);
  stepper.setAcceleration(1000.0);
  stepper.setCurrentPosition(0);

  pinMode(ledPin, OUTPUT);
  pinMode(endStop, INPUT_PULLUP);
  delay(1000);
  //calibrate();
  //delay(15000);
}


void sendPos(int nowPos) {
  OSCMessage msg("/pos");
  msg.add(nowPos);
  Udp.beginPacket(outIp, outPort);
  msg.send(Udp); // send the bytes to the SLIP stream
  Udp.endPacket(); // mark the end of the OSC Packet
  msg.empty(); // free space occupied by message
  delay(1);
}

void sendMessage(char* serialMessage) {
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
  stepper.moveTo(posN);

}

void dispense(OSCMessage &msg, int addrOffset){
  int posN =  stepper.currentPosition() + 400;
  stepper.moveTo(posN);

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
  stepper.run();
  POS =   stepper.currentPosition();
 
  if (stepper.distanceToGo() == 0) {
    if (stopped == false) {
      Serial.print("* ");
      Serial.println(stepper.currentPosition());
    }
    if (stopped == false) {
      stopped = true;
      sendMessage("OK");
    }
  }else{
//    Serial.println(stepper.distanceToGo());
    sendPos(POS);
  }
  if (Serial.available()) {
    static char buffer[80];
    while (Serial.available()) {
      if (readline(Serial.read(), buffer, 80) > 0) {
        sendMessage(buffer);
      }
    }
  }
  
  ESP.wdtFeed();
  OSCMessage msgIN;
  int size;
  if ( (size = Udp.parsePacket()) > 0)
  {
    while (size--)
      msgIN.fill(Udp.read());
    if (!msgIN.hasError()) {
      msgIN.route("/help", routeHelp);
      msgIN.route("/stats", routeStats);

      msgIN.route("/position", routePos);
      msgIN.route("/dispense", dispense);
      
      msgIN.route("/speed", routeSpeed);
      msgIN.route("/acc", routeAcc);
      msgIN.route("/calibrate", routeCalibrate);
//      msgIN.route("/capture", routeCapture);
    }
    outIp = Udp.remoteIP();
  }
  // Check if a client has connected
  WiFiClient client = server.available();
  if (client) {
    Serial.println("");
    Serial.println("New client");
    // Wait for data from client to become available
    while (client.connected() && !client.available()) {
      delay(1);
    }

    // Prepare the response. Start with the common header:
    String s = "HTTP/1.1 200 OK\r\n";
    s += "Content-Type: text/html\r\n\r\n";
    s += "<!DOCTYPE HTML>\r\n<html>\r\n";
    // s += "IP = ";


    s += "<head>";
    s += "<style>";
    s += "body {";
    s += "    font-family: arial, sans-serif;";

    s += "}";

    s += "table {";
    s += "    font-family: arial, sans-serif;";
    s += "    border-collapse: collapse;";
    s += "    width: 100%;";
    s += "}";

    s += "td, th {";
    s += "    border: 1px solid #dddddd;";
    s += "    text-align: left;";
    s += "    padding: 8px;";
    s += "}";

    s += "tr:nth-child(even) {";
    s += "    background-color: #dddddd;";
    s += "}";
    s += "</style>";
    s += "</head>";

    s += "<br>";

    s += "<table>";
    s += "  <tr>";
    s += "    <th></th>";
    s += "   <th>IP</th>";
    s += "   <th>PORT</th>";
    s += " </tr>";
    s += " <tr>";

    s += "  <td>";
    s += String(DeviceName);
    s += ".local</td>";

    String str = "";
    for (int i = 0; i < 4; i++)
      str += i  ? "." + String(WiFi.localIP()[i]) : String(WiFi.localIP()[i]);
    s += "  <td>";
    s += str;
    s += "</td>";

    s += "  <td>";
    s += localPort;
    s += "</td>";

    s += " </tr>";
    s += "  <tr>";

    s += "  <td>Connected To </td>";
    str = "";

    for (int i = 0; i < 4; i++) {
      str += i  ? "." + String(outIp[i]) : String(outIp[i]);
    }

    s += "  <td>";
    s += str;
    s += "</td>";

    s += "  <td>";
    s += outPort;
    s += "</td>";

    s += "  </tr>";
    s += "</table>";


    s += "<br>";
    s += "<br>";
    s += HELP;
    s += "<br>";
    s += "<br>";

    String mStr = "";
    mStr += "<table>";
    mStr += "  <tr>";
    mStr += "    <th>MS1</th>";
    mStr += "   <th>MS2</th>";
    mStr += "   <th>MS3</th>";
    mStr += "   <th>Resolution</th>";
    mStr += " </tr>";
    mStr += " <tr>";
    mStr += "  <td>0</td>";
    mStr += "  <td>0</td>";
    mStr += "  <td>0</td>";
    mStr += "  <td>FULL</td>";
    mStr += " </tr>";
    mStr += "  <tr>";
    mStr += "  <td>1</td>";
    mStr += "  <td>0</td>";
    mStr += "  <td>0</td>";
    mStr += "  <td>HALF</td>";
    mStr += "  </tr>";
    mStr += "  <tr>";
    mStr += "  <td>0</td>";
    mStr += "  <td>1</td>";
    mStr += "  <td>0</td>";
    mStr += "  <td>QUARTER</td>";
    mStr += "  </tr>";
    mStr += "<tr>";
    mStr += "  <td>1</td>";
    mStr += "  <td>1</td>";
    mStr += "  <td>0</td>";
    mStr += "  <td>EIGHTH</td>";
    mStr += "  </tr>";
    mStr += " <tr>";
    mStr += "  <td>1</td>";
    mStr += "  <td>1</td>";
    mStr += "  <td>1</td>";
    mStr += "  <td>SIXTEENTH</td>";
    mStr += "  </tr>";
    mStr += "</table>";

    s += mStr;

    s += "</html>\n";

    client.print(s);
    delay(1);
    Serial.println("Client disonnected");
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
