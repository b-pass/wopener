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

#define MOTOR_TIMEOUT 15625 // TIM_DIV256 = 3.2us per tick, so 50 millis = 15625 ticks
#define WINDOW_TIMEOUT 75000 // ms

#define WEB_PORT 80
#define MQTT_PORT 1883

ESP8266WebServer webServer(WEB_PORT);
WiFiClient wifiClient;

char mqttGetTopic[48];
char mqttDiscTopic[48];
char mqttSetTopic[48];
unsigned long lastDiscovery = 0;
Adafruit_MQTT_Client mqtt(&wifiClient, MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS);
Adafruit_MQTT_Subscribe hassSub(&mqtt, mqttSetTopic, 2);

volatile unsigned int encoderValue = 0;
volatile unsigned int lastEncoderValue = 0;
ICACHE_RAM_ATTR void handleEncoder()
{
  ++encoderValue;
  timer1_write(MOTOR_TIMEOUT);
}

ICACHE_RAM_ATTR void stopAll()
{
  digitalWrite(MOTOR1, LOW);
  digitalWrite(MOTOR2, LOW);
}

void ReqInfo();
void ReqMove();
void HassDiscovery();
void HassSet(uint32_t);

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
  timer1_isr_init();
  timer1_attachInterrupt(stopAll);
      
  Serial.println(F("WiFI begin " WIFI_SSID));
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoConnect(true);
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

  snprintf(mqttGetTopic, sizeof(mqttGetTopic), "homeassistant/sensor/huzzah/%06X/value", ESP.getChipId());
  snprintf(mqttDiscTopic, sizeof(mqttDiscTopic), "homeassistant/sensor/huzzah/%06X/config", ESP.getChipId());
  snprintf(mqttSetTopic, sizeof(mqttSetTopic), "homeassistant/sensor/huzzah/%06X/set", ESP.getChipId());
  hassSub.setCallback(HassSet);
  mqtt.subscribe(&hassSub);
  int ret = mqtt.connect();
  if (ret != 0) { // connect will return 0 for connected
    Serial.println(mqtt.connectErrorString(ret));
    mqtt.disconnect();
  }
  
  webServer.on("/", &ReqInfo);
  webServer.on("/move", &ReqMove);
  webServer.begin();

  int seed = 0;
  for (int i = 0; i < 32; ++i)
    seed = (analogRead(A0)&1) | (seed << 1);
  randomSeed(seed ? seed : millis());
  
  digitalWrite(RED_LED, HIGH); // off
  pinMode(MINI_BUTTON, INPUT);
  
  Serial.println(F("Setup finished"));
}

unsigned long motorStartMS = 0, motorCheckMS = 0;
void startMotor(int direction)
{
  if (motorStartMS)
    return; // already moving
  
  encoderValue = lastEncoderValue = 0;
  timer1_write(MOTOR_TIMEOUT);
  timer1_enable(TIM_DIV256, TIM_EDGE, TIM_LOOP);
  digitalWrite(direction == 1 ? MOTOR1 : MOTOR2, HIGH);
  motorCheckMS = millis();
  motorStartMS = motorCheckMS ? motorCheckMS : 1;
  
  Serial.print("Moving window, dir="); Serial.println(direction);
}

void checkMotor()
{
  if (!motorStartMS)
    return;
  
  auto now = millis();
  if ((now - motorCheckMS) < 50)
    return;

  if (encoderValue != lastEncoderValue && (now - motorStartMS) < WINDOW_TIMEOUT)
  {
    static bool led = false;
    led = !led;
    digitalWrite(RED_LED, led ? LOW : HIGH);
    lastEncoderValue = encoderValue;
    motorCheckMS = now;
  }
  else
  {
    stopAll();
    timer1_disable();
    digitalWrite(RED_LED, HIGH);
    motorCheckMS = 0;
    
    Serial.print("Stopped window after ");
    Serial.print(now - motorStartMS);
    Serial.print(" ms and ");
    Serial.print(encoderValue);
    Serial.print(" encoder ticks");
    Serial.println("!");
  }
}
  
void loop(void)
{
  webServer.handleClient();
  mqtt.readSubscription(10);
  checkMotor();

  /*if (WiFi.status() != WL_CONNECTED && motorStartMS == 0)
    ESP.reset(); // reboot me, my firmware sucks!
  */
  
  if (!mqtt.connected())
  {
    Serial.print("Re-connecting to MQTT... ");
    int ret = mqtt.connect();
    if (ret != 0) { // connect will return 0 for connected
      Serial.println(mqtt.connectErrorString(ret));
      mqtt.disconnect();
    }
    else {
      Serial.println("MQTT Connected!");
      HassDiscovery();
    }
  }
  else
  {
    if ((millis() - lastDiscovery) >= 300000)
      HassDiscovery();
  }
  
  if (digitalRead(MINI_BUTTON) == LOW)
  {
    static int direction = 0;
    direction = !direction;
    startMotor(direction);
  }
}

void ReqInfo()
{
  Serial.println(F("info requested"));
  
  String doc;

  doc += F("Huzzah Window Controller.\n\n");
  doc += F("Serial:"); doc += String(ESP.getChipId(), HEX); doc += F("\n");
  doc += F("Now:"); doc += millis(); doc += F("\n");
  
  doc += F("\n\n");
  webServer.send(200, "text/plain", doc);
}

void ReqMove()
{
  static int direction = 0;
  direction = !direction;
  
  Serial.println(F("/move requested"));
  
  String doc;
  doc += F("Moving!\nNew Direction: ");
  doc += direction;
  doc += F("\n\n");
  
  webServer.send(200, "text/plain", doc);

  startMotor(direction);
}

void HassDiscovery()
{
  Serial.print(F("Hass Discovery "));
  String cfg = F("{\"dev_cla\":\"window\","
                 "\"name\":\"Window ");
  cfg += String(ESP.getChipId(), HEX);
  cfg += "\",\"uniq_id\":\"";
  cfg += String(ESP.getChipId(), HEX);
  cfg += "\",\"stat_t\":\"";
  cfg += mqttGetTopic;
  cfg += "\",\"cmd_t\":\"";
  cfg += mqttSetTopic;
  cfg += "\"}";

  mqtt.publish(mqttDiscTopic, cfg.c_str());
  Serial.println(F("Sent!"));
  lastDiscovery = millis();
}

void HassSet(uint32_t direction)
{
  startMotor(direction);
}
