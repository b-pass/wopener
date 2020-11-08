#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <Adafruit_MQTT_Client.h>
#include "secrets.h"

#define RED_LED 0
#define MINI_BUTTON 0
#define BLUE_LED 2
#define MOTOR1 12
#define MOTOR2 13
#define ENCODER1 4
#define ENCODER2 5

#define WEB_PORT 80
#define MQTT_PORT 1883

ESP8266WebServer webServer(WEB_PORT);
WiFiClient wifiClient;
Adafruit_MQTT_Client mqtt(&wifiClient, MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS);

char mqttTopic[64];
char mqttDiscTopic[64];

volatile unsigned int encoderValue = 0;
ICACHE_RAM_ATTR void handleEncoder() { ++encoderValue; }

void MQTT_connect();
void ReqDebug();
void HassDiscovery();

void setup(void) {
  pinMode(RED_LED, OUTPUT);
  digitalWrite(RED_LED, LOW); // on

  Serial.begin(115200);
  delay(25);
  Serial.println(F("\nSetup..."));
  
  pinMode(BLUE_LED, OUTPUT);
  digitalWrite(BLUE_LED, LOW);

  pinMode(MOTOR1, OUTPUT);
  digitalWrite(MOTOR1, LOW);
  pinMode(MOTOR2, OUTPUT);
  digitalWrite(MOTOR2, LOW);

  pinMode(ENCODER1, INPUT);
  pinMode(ENCODER2, INPUT);

  attachInterrupt(digitalPinToInterrupt(ENCODER1), handleEncoder, RISING);
  attachInterrupt(digitalPinToInterrupt(ENCODER2), handleEncoder, RISING);
  
  Serial.println(F("WiFI begin " WIFI_SSID));
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(BLUE_LED, LOW); // on
    delay(250);
    digitalWrite(BLUE_LED, HIGH); // off
    delay(250);
    Serial.print(".");
  }
  WiFi.setAutoReconnect(true);
  Serial.print(F("  Connected, IP: "));
  Serial.println(WiFi.localIP());
  
  int ret = mqtt.connect();
  if (ret != 0) { // connect will return 0 for connected
    Serial.println(mqtt.connectErrorString(ret));
    mqtt.disconnect();
  }
  
  webServer.on("/", [](){ Serial.println(F("root requested")); webServer.send(200, "text/plain", "Welcome!"); });
  webServer.on("/debug", &ReqDebug);
  webServer.begin();

  snprintf(mqttTopic, sizeof(mqttTopic), "homeassistant/sensor/huzzah/%06X/value", ESP.getChipId());
  snprintf(mqttDiscTopic, sizeof(mqttDiscTopic), "homeassistant/sensor/huzzah/%06X/config", ESP.getChipId());

  int seed = 0;
  for (int i = 0; i < 32; ++i)
    seed = (analogRead(A0)&1) | (seed << 1);
  randomSeed(seed ? seed : millis());
  
  digitalWrite(RED_LED, HIGH); // off
  pinMode(MINI_BUTTON, INPUT);
  
  Serial.println(F("Setup finished"));
}

#define WINDOW_TIMEOUT 75000
void activate(int direction)
{
  encoderValue = 0;
  uint32_t oldEnc = 0;
  Serial.print("Moving window, dir="); Serial.println(direction);
  auto start = millis();
  digitalWrite(direction == 1 ? MOTOR1 : MOTOR2, HIGH);
  while (abs(millis() - start) < WINDOW_TIMEOUT)
  {
    digitalWrite(BLUE_LED, HIGH);
    oldEnc = encoderValue;
    delay(50);
    if (encoderValue == oldEnc)
      break;
    oldEnc = encoderValue;
    digitalWrite(BLUE_LED, LOW);
  }
  digitalWrite(BLUE_LED, LOW);
  digitalWrite(MOTOR1, LOW);
  digitalWrite(MOTOR2, LOW);
  Serial.print("Stopped window after ");
  Serial.print(millis() - start);
  Serial.print(" ms and ");
  Serial.print(encoderValue);
  Serial.print(" encoder ticks");
  Serial.println("!");
}

  /*if (WiFi.status() != WL_CONNECTED)
    ESP.reset(); // reboot me, my firmware sucks!
  
  if (!mqtt.connected())
  {
    digitalWrite(RED_LED, LOW); // on
    Serial.print("Re-connecting to MQTT... ");
    int ret = mqtt.connect();
    if (ret != 0) { // connect will return 0 for connected
      Serial.println(mqtt.connectErrorString(ret));
      mqtt.disconnect();
      
      digitalWrite(RED_LED, HIGH); // off
      delay(500);
      digitalWrite(RED_LED, LOW); // on
      delay(1500);
    }
    else {
      Serial.println("MQTT Connected!");
      digitalWrite(RD_LED, HIGH); // off
      
      HassDiscovery();
    }
  }*/
  
void loop(void) {
  delay(1);
  
  webServer.handleClient();


  static int direction = 1;

  if (digitalRead(MINI_BUTTON) == LOW)
  {
    activate(direction);
    direction = !direction;
  } 
}

void ReqDebug()
{
  Serial.println(F("/debug requested"));
  
  String doc;

  doc += F("Serial:"); doc += String(ESP.getChipId(), HEX); doc += F("\n");
  doc += F("Now:"); doc += millis(); doc += F("\n");
  
  doc += F("\n\n");
  webServer.send(200, "text/plain", doc);
}

void HassDiscovery()
{
  Serial.print(F("Hass Discovery "));
  
  /*String cfg = "{\"dev_cla\":\"window\","
                "\"name\":\"Window Sensor ";
  cfg += String(ESP.getChipId(), HEX);
  cfg += "\",\"uniq_id\":\"";
  cfg += String(ESP.getChipId(), HEX);
  cfg += "\",\"stat_t\":\"";
  cfg += mqttTopic;
  cfg += "\"}";

  mqtt.publish(mqttDiscTopic, cfg.c_str());
  Serial.println(F("Sent!"));*/
}
