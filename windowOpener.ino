#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <Adafruit_MQTT_Client.h>
#include "secrets.h"

#define LEFT_HANDED

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

char mqttDiscTopic[48];
char mqttStateTopic[48];
char mqttCmdTopic[48];
char mqttAvailTopic[48];
unsigned long lastDiscovery = 0;
Adafruit_MQTT_Client mqtt(&wifiClient, MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS);
Adafruit_MQTT_Subscribe hassSub(&mqtt, mqttCmdTopic);

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
void HassCommand(char *, uint16_t);

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

  snprintf(mqttDiscTopic, sizeof(mqttDiscTopic), "homeassistant/cover/wopener/%06X/config", ESP.getChipId());
  snprintf(mqttAvailTopic, sizeof(mqttAvailTopic), "homeassistant/cover/wopener/%06X/avail", ESP.getChipId());
  snprintf(mqttStateTopic, sizeof(mqttStateTopic), "homeassistant/cover/wopener/%06X/get", ESP.getChipId());
  snprintf(mqttCmdTopic, sizeof(mqttCmdTopic), "homeassistant/cover/wopener/%06X/set", ESP.getChipId());
  hassSub.setCallback(HassCommand);
  mqtt.subscribe(&hassSub);
  mqtt.will(mqttAvailTopic, "offline");
  int ret = mqtt.connect();
  if (ret != 0) { // connect will return 0 for connected
    Serial.println(mqtt.connectErrorString(ret));
    mqtt.disconnect();
  }
  
  webServer.on("/", &ReqInfo);
  webServer.on("/open", &ReqOpen);
  webServer.on("/close", &ReqClose);
  webServer.on("/stop", &ReqStop);
  webServer.begin();

  int seed = 0;
  for (int i = 0; i < 32; ++i)
    seed = (analogRead(A0)&1) | (seed << 1);
  randomSeed(seed ? seed : millis());
  
  digitalWrite(RED_LED, HIGH); // off
  pinMode(MINI_BUTTON, INPUT);
  
  Serial.println(F("Setup finished"));
}

#ifdef LEFT_HANDED
#define DIR_OPEN 0
#define DIR_CLOSE 1
#else
#define DIR_OPEN 1
#define DIR_CLOSE 0
#endif

unsigned long motorStartMS = 0, motorCheckMS = 0;
void startMotor(int direction)
{
  if (motorStartMS)
    return; // already moving

  stopAll();
  encoderValue = lastEncoderValue = 0;
  timer1_write(MOTOR_TIMEOUT);
  timer1_enable(TIM_DIV256, TIM_EDGE, TIM_SINGLE);
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
    lastEncoderValue = encoderValue;
    motorCheckMS = now;
  }
  else
  {
    stopAll();
    timer1_disable();
    
    Serial.print("Stopped window after ");
    Serial.print(now - motorStartMS);
    Serial.print(" ms and ");
    Serial.print(encoderValue);
    Serial.print(" encoder ticks");
    Serial.println("!");
    
    motorStartMS = 0;
  }
}
  
void loop(void)
{
  webServer.handleClient();
  mqtt.readSubscription(10);
  checkMotor();
  
  if (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(BLUE_LED, LOW); // on
    Serial.println("NOT CONNECTED :(");
    Serial.print("wifi status = ");
    Serial.println(WiFi.status());
    delay(100);
    digitalWrite(BLUE_LED, HIGH); // off
    delay(100);
  }

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
    stopAll();
    motorStartMS = 0;
    startMotor(direction);
  }
}

void ReqInfo()
{
  Serial.println(F("info requested"));
  
  String doc;

  doc += R"html(
<!DOCTYPE html>
<html>
<head><title>WOpener</title>
<script>
function xhr(it) {
  var xhttp = new XMLHttpRequest();
  xhttp.open("GET", it, true);
  xhttp.send();
}
</script>
</head>
<body>
<h1>WOpener - Window Controller v1.0</h1>
<button type="button" onclick="xhr('/open')">Open</button> <button type="button" onclick="xhr('/close')">Close</button> <button type="button" onclick="xhr('/stop')">STOP</button>
<br /><br />
)html";
  doc += "Serial: <b>"; doc += String(ESP.getChipId(), HEX); doc += "</b><br />\n";
  doc += "Now: <b>"; doc += millis(); doc += "</b><br />\n";
  doc += "</body></html>";

  webServer.send(200, "text/html", doc);
}

void ReqOpen()
{
  Serial.println(F("/open requested"));
  
  String doc;
  doc += F("Moving in direction: ");
  doc += DIR_OPEN;
  doc += F("\n\n");
  
  webServer.send(200, "text/plain", doc);

  startMotor(DIR_OPEN);
}

void ReqClose()
{
  Serial.println(F("/close requested"));
  
  String doc;
  doc += F("Moving in direction: ");
  doc += DIR_CLOSE;
  doc += F("\n\n");
  
  webServer.send(200, "text/plain", doc);

  startMotor(DIR_CLOSE);
}

void ReqStop()
{
  Serial.println(F("/stop requested"));
  
  String doc;
  doc += F("Stopping...\n");
  if (!motorStartMS)
    doc += F("I think it was already stopped, but I'll try anyway\n");
  
  webServer.send(200, "text/plain", doc);

  stopAll();
}

void HassDiscovery()
{
  Serial.println(F("Sending Hass Discovery & availability"));
  
static char const PROGMEM cfgTemplate[] = R"json({
"dev_cla":"window",
"name":"Window Opener %06X",
"uniq_id":"%06X",
"avty_t":"%s",
"cmd_t":"%s",
"stat_t":"%s",
"pl_cls":"close",
"pl_open":"open",
"pl_stop":"stop"
})json";

  char cfg[sizeof(cfgTemplate)+16*2+48*3];
  snprintf(cfg, sizeof(cfg), cfgTemplate, 
    ESP.getChipId(),
    ESP.getChipId(),
    mqttAvailTopic,
    mqttCmdTopic,
    mqttStateTopic);

  mqtt.publish(mqttDiscTopic, cfg);
  
  mqtt.publish(mqttAvailTopic, "online");
  
  lastDiscovery = millis();
}

void HassCommand(char *cmd, uint16_t len)
{
  if (len < 1 || !cmd)
    return;
  else if (toupper(cmd[0]) == 'S')
    stopAll();
  else if (toupper(cmd[0]) == '0')
    startMotor(DIR_OPEN);
  else if (toupper(cmd[0]) == 'C')
    startMotor(DIR_CLOSE);
}
