#include <ESP8266WiFi.h>
#include <Ticker.h>  //Ticker Library
#include <WiFiUdp.h>
#include <WiFiManager.h> // --> https://github.com/tzapu/WiFiManager
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>

#define HOSTNAME "WifiLightSensor"
#define LOW_LIGHT_LEVEL 400
#define RED_LEVEL 0xff
#define GREEN_LEVEL 0xff
#define BLUE_LEVEL 0x59

const IPAddress broadcastIp(192, 168, 0, 255);
const IPAddress netmask(255, 255, 255, 0);

const int noBulbs = 2;
struct bulb {
  String ip;
  String id;
  String model;
  byte power;
  byte warm_white;
  byte cool_white;
  byte red;
  byte green;
  byte blue;
  byte mode_value;
  byte bulb_type;
  byte version_no;
  byte slowness;
};

byte POWER_ON = 0x23;
byte POWER_OFF = 0x24;
byte WHITE_VALUE = 0x40;

const int lightHTTPPort = 5577; // Port to communicate with PLC
const unsigned int replyPort = 48899;      // local port to listen on
const long updateLightInterval = 60; //check sensir every minute
const int syncLightInterval = 300; //check light sync every five minutes

Ticker updateLightSync;
Ticker updateLightMeasurement;

bool isCheckLightSensor = false;
bool isLightsOn = true;
bool isFirstCheck = true;
bool isSyncTime = false;

bulb bulbs[noBulbs];

int sensorValue = 0;

long lastBroadcast = 0L;
char packetBuffer[255]; //buffer to hold incoming packet
String discoveryMessage = "HF-A11ASSISTHREAD\r";       // a string to send back
WiFiUDP Udp;

ESP8266WebServer httpServer(80);

WiFiClient client;
const byte power_on[4] = {0x71, 0x23, 0x0f, 0xA3}; // This array contains all HEX code to send
const byte power_off[4] = {0x71, 0x24, 0x0f, 0xA4};
const byte warmwhite[8] = {0x31, 0x00, 0x00, 0x00, 0x40, 0x00, 0x0f, 0x80};
const byte bulb_status[4] = {0x81, 0x8a, 0x8b, 0x96};

void setup() {
  Serial.begin(115200);
  delay(10);
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(90);
  if (!wifiManager.startConfigPortal(HOSTNAME)) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    if (!wifiManager.autoConnect(HOSTNAME)) {
      ESP.reset();
      delay(5000);
    }
  }
  while ( WiFi.status() != WL_CONNECTED ) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("connected...yeey :)");
  if (!MDNS.begin(HOSTNAME)) {
    Serial.println("Error setting up MDNS responder!");
  }
  WiFi.mode(WIFI_STA);

  httpServer.on("/", HTTP_GET, []() {
    yield();
    httpServer.sendHeader("Connection", "close");
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    Serial.println("Serving up HTML...");
    String html = "<html><body>Light Sensor: ";
    html += sensorValue;
    html += "<br>";
    html += "<br></body></html>";
    Serial.print("Done serving up HTML...");
    Serial.println(html);
    httpServer.send(200, "text/html", html);
  });
  WiFi.hostname(String(HOSTNAME));

  // print the received signal strength:
  updateLightMeasurement.attach(updateLightInterval, shouldUpdateLightSensor);
  updateLightSync.attach(syncLightInterval, shouldUpdateSync);
  Serial.println(analogRead(A0));
  httpServer.begin();

}

void loop() {
  if (isCheckLightSensor) {
    sensorValue = analogRead(A0);
    Serial.println(sensorValue);
    delay(1000);
    int secondValue = analogRead(A0);
    if (abs(sensorValue - secondValue) / sensorValue < .1) {
      isCheckLightSensor = false;
      if (sensorValue > LOW_LIGHT_LEVEL && (isLightsOn || isFirstCheck)) {
        findBulbs();
        for (int x = 0; x < noBulbs; x++) {
          turnOff(bulbs[x]);
          delay(1000);
        }
        isLightsOn = false;
        isFirstCheck = false;
      } else if (sensorValue < LOW_LIGHT_LEVEL && (!isLightsOn || isFirstCheck)) {
        findBulbs();
        for (int x = 0; x < noBulbs; x++) {
          turnOn(bulbs[x]);
          delay(1000);
          setBulbColor(bulbs[x], RED_LEVEL, GREEN_LEVEL, BLUE_LEVEL);
          delay(1000);
        }
        isLightsOn = true;
        isFirstCheck = false;
      }

    }
    if (isSyncTime) {
      syncLights();
      isSyncTime = false;
    }
  }
  httpServer.handleClient();
}

void shouldUpdateLightSensor() {
  isCheckLightSensor = true;
}

void shouldUpdateSync() {
  isSyncTime = true;
}

bool connectToLight(bulb light) {
  for (int i = 0; i < 10; i++) {
    if (!client.connect(light.ip, lightHTTPPort)) {
      Serial.println("connection failed");
      delay(15000);
    } else {
      return true;
    }
  }
  return false;
}

void findBulbs() {
  int y = 0;
  while ( y < noBulbs) {
    Udp.begin(replyPort);
    for (int z = 0; z < 2; z++) {
      Serial.println("Sending UDP broadcast...");
      Udp.beginPacket(broadcastIp, replyPort); // subnet Broadcast IP and port
      byte buf2[18];
      discoveryMessage.getBytes(buf2, 18);
      Udp.write(buf2, 17);
      Udp.endPacket();
    }
    Serial.println("Listening...");
    lastBroadcast = millis();
    while (millis() - lastBroadcast < 10000) {
      // put your main code here, to run repeatedly:
      int packetSize = Udp.parsePacket();
      if (packetSize) {
        // read the packet into packetBufffer
        int len = Udp.read(packetBuffer, 255);
        if (len > 0) {
          packetBuffer[len] = 0;

          String temp = "";
          int x = 0;
          bulb newBulb;

          for (int i = 0; i < len; i++) {
            if (packetBuffer[i] == ',' || i == len - 1 ) {
              //             Serial.println("Temp: " + temp);
              //           Serial.println(x);
              switch (x) {
                case 0:
                  newBulb.ip = temp;
                  break;
                case 1:
                  newBulb.id = temp;
                  break;
                case 2:
                  newBulb.model = temp;
                  break;
              }
              x++;
              temp = "";
              if (x == 3 ) {
                bulbs[y++] = newBulb;
              }
            } else {
              temp += packetBuffer[i];
            }
          }
        }
      }
    }
  }
}

void setWarmWhite(bulb bulb) {
  //    Serial.println(generateCheckSum(warmwhite, 8));
  //Serial.println(warmwhite[7]);
  connectToLight(bulb);
  client.write(warmwhite, sizeof(warmwhite));
  Serial.println("Set soft white for " + bulb.ip);

}

void turnOff(bulb current) {
  if (connectToLight(current)) {
    Serial.println("Turned off " + current.ip);
    client.write (power_off, sizeof(power_off));
    client.stop();
  }
}

void turnOn(bulb current) {
  if (connectToLight(current)) {
    Serial.println("Turned on");
    client.write(power_off, sizeof(power_off));
    Serial.println("Turned on " + current.ip);
    client.stop();
  }
}

void syncLights() {

  byte reply_data[14];
  for (int x = 0; x < noBulbs; x++) {
    connectToLight(bulbs[x]);
    client.write(bulb_status, sizeof(bulb_status));
    if (client.connected()) {
      size_t len = client.available();
      client.readBytes(reply_data, sizeof(reply_data));
      bulbs[x].bulb_type = reply_data[1];
      bulbs[x].power = reply_data[2];
      bulbs[x].mode_value = reply_data[3];
      bulbs[x].slowness = reply_data[5];
      bulbs[x].red = reply_data[6];
      bulbs[x].green = reply_data[7];
      bulbs[x].blue = reply_data[8];
      bulbs[x].warm_white = reply_data[9];
      bulbs[x].version_no = reply_data[10];
      bulbs[x].cool_white = reply_data[11];
    }
    delay(500);
  }
  for (int x = 0; x < noBulbs - 1; x++) {
    if (bulbs[x].power != bulbs[x + 1].power) {
      if (sensorValue < LOW_LIGHT_LEVEL) {
        if (bulbs[x].power = POWER_ON) {
          turnOn(bulbs[x + 1]);
        } else {
          turnOn(bulbs[x]);
        }
      } else {
        if (bulbs[x].power == POWER_OFF) {
          Serial.println(bulbs[x + 1].power);
          turnOff(bulbs[x + 1]);
        } else {
          Serial.println(bulbs[x].power);
          turnOff(bulbs[x]);
        }

      }
    }
    if (((bulbs[x].red == RED_LEVEL && bulbs[x].green == GREEN_LEVEL && bulbs[x].blue == BLUE_LEVEL ) && (bulbs[x + 1].red != RED_LEVEL || bulbs[x + 1].green == GREEN_LEVEL || bulbs[x + 1].blue == BLUE_LEVEL))
        || ((bulbs[x + 1].red == RED_LEVEL && bulbs[x + 1].green == GREEN_LEVEL && bulbs[x + 1].blue == BLUE_LEVEL ) && (bulbs[x].red != RED_LEVEL || bulbs[x].green == GREEN_LEVEL || bulbs[x].blue == BLUE_LEVEL))) {
      setBulbColor(bulbs[x], RED_LEVEL, GREEN_LEVEL, BLUE_LEVEL);
      setBulbColor(bulbs[x + 1], RED_LEVEL, GREEN_LEVEL, BLUE_LEVEL);
    }
  }
}

void setBulbColor(bulb current, byte red, byte green, byte blue) {
  byte colors[9] = {0x31, red, green, blue, 0, 0, 0xf0, 0x0f};
  colors[8] = generateCheckSum(colors, 9);
  if (connectToLight(current)) {
    client.write(colors, sizeof(colors));
    Serial.println("Set Colors for " + current.ip);
    client.stop();
  } else {
    Serial.println("Could not connect....");
  }
}

int generateCheckSum(byte input[], int arraySize) {
  int checksum = 0;
  for (int i = 0; i < arraySize - 1; i++) {
    checksum += input[i];
  }
  checksum &= 0xFF;
  Serial.print("Checksum: ");
  Serial.println(checksum);
  return checksum;
}
