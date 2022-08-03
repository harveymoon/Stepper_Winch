// A basic everyday NeoPixel strip test program.

// NEOPIXEL BEST PRACTICES for most reliable operation:
// - Add 1000 uF CAPACITOR between NeoPixel strip's + and - connections.
// - MINIMIZE WIRING LENGTH between microcontroller board and first pixel.
// - NeoPixel strip's DATA-IN should pass through a 300-500 OHM RESISTOR.
// - AVOID connecting NeoPixels on a LIVE CIRCUIT. If you must, ALWAYS
//   connect GROUND (-) first, then +, then data.
// - When using a 3.3V microcontroller with a 5V-powered NeoPixel strip,
//   a LOGIC-LEVEL CONVERTER on the data line is STRONGLY RECOMMENDED.
// (Skipping these may work OK on your workbench but can fail in the field)

#include <Adafruit_NeoPixel.h>
// Which pin on the Arduino is connected to the NeoPixels?
// On a Trinket or Gemma we suggest changing this to 1:
#define persistence_pin    D3

// How many NeoPixels are attached to the Arduino?
#define LED_COUNT 60

// Declare our NeoPixel strip object:
Adafruit_NeoPixel strip(LED_COUNT, persistence_pin, NEO_GRB + NEO_KHZ800);
// Argument 1 = Number of pixels in NeoPixel strip
// Argument 2 = Arduino pin number (most are valid)
// Argument 3 = Pixel type flags, add together as needed:
//   NEO_KHZ800  800 KHz bitstream (most NeoPixel products w/WS2812 LEDs)
//   NEO_KHZ400  400 KHz (classic 'v1' (not v2) FLORA pixels, WS2811 drivers)
//   NEO_GRB     Pixels are wired for GRB bitstream (most NeoPixel products)
//   NEO_RGB     Pixels are wired for RGB bitstream (v1 FLORA pixels, not v2)
//   NEO_RGBW    Pixels are wired for RGBW bitstream (NeoPixel RGBW products)



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


#include <Adafruit_NeoPixel.h>



const char* DeviceName = "Persistence";//"stepper_winch_01";

char* HELP = "/help, /calibrate, /stats, /microstep 0,0,0, /position 100, /speed 100, /acc 1000, /capture";

///MS1    MS2   MS3   Microstep Resolution
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
const unsigned int outPort = 9000;              //host port
const unsigned int localPort = 9999;        // local port to listen for UDP packets (here's where we send the packets)



AccelStepper stepper(1, D5, D0); // mode, step, dir
unsigned long ACC = 100;
unsigned int SPEED = 1000;
unsigned long POS;


//byte microStep[] = {LOW, LOW, LOW};

boolean stopped = true;

//const byte ms1 = D6;
//const byte ms2 = D7;
//const byte ms3 = D8;

const byte endStop = D4;

const int EnablePin = D7;

const int ledPin =  BUILTIN_LED;      // the number of the LED pin


void setup() {


  Serial.begin(115200);

  Serial.println("STARTING");

  //  pinMode(ms1, OUTPUT);
  //  pinMode(ms2, OUTPUT);
  //  pinMode(ms3, OUTPUT);

  pinMode(EnablePin, OUTPUT);

  //  pinMode(relayPin, OUTPUT);

  //  int one = microStep[0];
  //  int two = microStep[1];
  //  int three = microStep[2];
  //  digitalWrite(ms1, LOW); // setPin
  //  digitalWrite(ms2, LOW); // setPin
  //  digitalWrite(ms3, LOW); // setPin

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

  stepper.setMaxSpeed(800);
  //  stepper.setSpeed(800);
  stepper.setSpeed(100);
  stepper.setAcceleration(100000.0);
  stepper.setCurrentPosition(0);

  strip.begin();           // INITIALIZE NeoPixel strip object (REQUIRED)
  strip.show();            // Turn OFF all pixels ASAP
  strip.setBrightness(100); // Set BRIGHTNESS to about 1/5 (max = 255)

  pinMode(ledPin, OUTPUT);
  pinMode(endStop, INPUT_PULLUP);
  delay(1000);
  //calibrate();
  //delay(15000);
}

void doCapture() {
  //  digitalWrite(relayPin, HIGH); // turn on relay with voltage HIGH
  //  delay(250);              // pause
  //  digitalWrite(relayPin, LOW);  // turn off relay with voltage LOW
  //  sendMessage("OK");
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

// rainbow(10);             // Flowing rainbow cycle along the whole strip
//  theaterChaseRainbow(50); // Rainbow-enhanced theaterChase variant

// for(int i=0; i<strip.numPixels(); i++) { // For each pixel in strip...
    uint32_t color = strip.Color(  random(255),   random(255), random(255));
    strip.setPixelColor(random(strip.numPixels()), color);         //  Set pixel's color (in RAM)
    strip.show();                          //  Update strip to match
//    delay(wait);                           //  Pause for a moment
//  }


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
    }

//  stepper.runSpeed();

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


      msgIN.route("/speed", routeSpeed);
      msgIN.route("/acc", routeAcc);
      msgIN.route("/calibrate", routeCalibrate);

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
  stepper.setSpeed(10);
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
