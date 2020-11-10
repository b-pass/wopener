#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <PubSubClient.h>
#include "secrets.h"

#define RED_LED 0
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

unsigned long lastDiscovery = 0;
void HassDiscovery();
void HassCommand(char *topic, byte *payload, unsigned int len);
char mqttDiscTopic[48];
char mqttStateTopic[48];
char mqttCmdTopic[48];
char mqttAvailTopic[48];
PubSubClient mqtt(MQTT_HOST, MQTT_PORT, HassCommand, wifiClient);

bool CurrentlyOpen = false;
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
void ReqSetConfig();
void ReqGetConfig();
void ReqOpen();
void ReqClose();
void ReqStop();

struct config_t {
  int WriteCount = 0;
  int Version = 1;
  bool RightHanded = false;
} Config;

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

  EEPROM.begin(256);
  EEPROM.get(0, Config);

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
  mqtt.setBufferSize(1024);
  
  webServer.on("/", &ReqInfo);
  webServer.on("/open", &ReqOpen);
  webServer.on("/close", &ReqClose);
  webServer.on("/stop", &ReqStop);
  webServer.on("/config", &ReqGetConfig);
  webServer.on("/set_config", &ReqSetConfig);
  webServer.begin();

  int seed = 0;
  for (int i = 0; i < 32; ++i)
    seed = (analogRead(A0)&1) | (seed << 1);
  randomSeed(seed ? seed : millis());
  
  digitalWrite(RED_LED, HIGH); // off
  
  Serial.println(F("Setup finished"));

  //closeWindow();
}

unsigned long motorStartMS = 0, motorCheckMS = 0;
void preStart()
{ 
  stopAll();
  if (motorStartMS)
    delay(25); // was it already moving? pause a little
  
  timer1_write(MOTOR_TIMEOUT);
  timer1_enable(TIM_DIV256, TIM_EDGE, TIM_SINGLE);
  motorCheckMS = millis();
  motorStartMS = motorCheckMS ? motorCheckMS : 1;
  encoderValue = lastEncoderValue = 0;
  
  digitalWrite(RED_LED, LOW); // on
}

void openWindow()
{
  if (motorStartMS && CurrentlyOpen)
    return; // already moving
  
  preStart();
  digitalWrite(Config.RightHanded ? MOTOR1 : MOTOR2, HIGH);
  
  CurrentlyOpen = true;
  mqtt.publish(mqttStateTopic, "opening");
  Serial.print("Opening window");
}

void closeWindow()
{
  if (motorStartMS && !CurrentlyOpen)
    return; // already moving
  
  preStart();
  digitalWrite(Config.RightHanded ? MOTOR2 : MOTOR1, HIGH);
  
  CurrentlyOpen = false;
  mqtt.publish(mqttStateTopic, "closing");
  Serial.print("Closing window");
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
    digitalWrite(RED_LED, HIGH); // off
    
    Serial.print("Stopped window after ");
    Serial.print(now - motorStartMS);
    Serial.print(" ms and ");
    Serial.print(encoderValue);
    Serial.print(" encoder ticks");
    Serial.println("!");
    
    motorStartMS = 0;
    
    mqtt.publish(mqttStateTopic, CurrentlyOpen ? "open" : "closed");
  }
}

void loop(void)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(BLUE_LED, LOW); // on
    Serial.println("NOT CONNECTED :(");
    Serial.print("wifi status = ");
    Serial.println(WiFi.status());
    delay(100);
    digitalWrite(BLUE_LED, HIGH); // off
  }
  
  webServer.handleClient();
  checkMotor();

  if (!mqtt.loop())
  {
    Serial.print("Re-connecting to MQTT... ");
    Serial.println(mqtt.state());

    char clientID[16];
    snprintf(clientID, sizeof(clientID), "wopener-%06X", ESP.getChipId());
    
    if (!mqtt.connect(clientID, MQTT_USER, MQTT_PASS, mqttAvailTopic, 0, false, "offline"))
    {
      Serial.print("MQTT connect failed: ");
      Serial.println(mqtt.state());
    }
    else
    {
      Serial.println("MQTT Connected!");
      mqtt.subscribe(mqttCmdTopic, 1);
      HassDiscovery();
    }
  }
  else
  {
    if ((millis() - lastDiscovery) >= 600000)
      HassDiscovery();
  }
}

void ReqInfo()
{
  Serial.println(F("info requested"));
  
  if (!webServer.authenticate(WEB_USER, WEB_PASS)) {
    webServer.requestAuthentication();
    return;
  }
    
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
<a href="/config">View/edit configuration</a><br /><br />
)html";
  doc += "<br /><h2>Rght now I think the window is: <b>"; doc += CurrentlyOpen ? "open" : "closed"; doc += "</b></h2><br /><br />\n";
  doc += "Serial: <b>"; doc += String(ESP.getChipId(), HEX); doc += "</b><br />\n";
  doc += "Now: <b>"; doc += millis(); doc += "</b><br />\n";
  doc += "</body></html>";

  webServer.send(200, "text/html", doc);
}

void ReqOpen()
{
  Serial.println(F("/open requested"));
  
  if (!webServer.authenticate(WEB_USER, WEB_PASS)) {
    webServer.requestAuthentication();
    return;
  }
  
  openWindow();
  webServer.send(200, "text/plain", "OPENING");
}

void ReqClose()
{
  Serial.println(F("/close requested"));
  
  if (!webServer.authenticate(WEB_USER, WEB_PASS)) {
    webServer.requestAuthentication();
    return;
  }
  
  closeWindow();
  webServer.send(200, "text/plain", "CLOSING");
}

void ReqStop()
{
  stopAll();
  Serial.println(F("/stop requested"));
  webServer.send(200, "text/plain", "STOPPING");
}

void ReqGetConfig()
{
  Serial.println(F("/config requested"));
  
  if (!webServer.authenticate(WEB_USER, WEB_PASS)) {
    webServer.requestAuthentication();
    return;
  }
  
  String form;

  form += R"html(
<!DOCTYPE html>
<html>
<head><title>WOpener Config</title></head>
<body>
<h1>WOpener - Configuration</h1>
<br /><br />Write Count: <b>)html";
  form += Config.WriteCount;
  form += R"html(</b>
<br /><br />
<form method="PORT" action="/set_config">
<input type="checkbox" name="RightHanded" id="rh" value="1" )html";
  if (Config.RightHanded) form += " checked ";
  form += R"html( /><label for="rh">Right Handed (opens clockwise)</label>
<br />
<input type="submit" value="Save to EEPROM" /><br />
</form></body></html>)html";

  webServer.send(200, "text/html", form);
}

void ReqSetConfig()
{
  Serial.println(F("/set_config requested"));
  
  if (!webServer.authenticate(WEB_USER, WEB_PASS)) {
    webServer.requestAuthentication();
    return;
  }

  Config.RightHanded = webServer.hasArg("RightHanded") && webServer.arg("RightHanded") == "1";

  Config.WriteCount++;
  EEPROM.put(0, Config);
  EEPROM.commit();
  
  webServer.send(200, "text/plain", "OK, config written\n");
  Serial.println(F("Config written."));
}

void HassDiscovery()
{
  Serial.println(F("Sending Hass Discovery & availability"));
  
  char discoveryMsg[512];
  snprintf(discoveryMsg, sizeof(discoveryMsg),
    R"json({"dev_cla":"window","name":"Window Opener %06X","uniq_id":"%06X","avty_t":"%s","cmd_t":"%s","stat_t":"%s","pl_cls":"close","pl_open":"open","pl_stop":"stop"})json", 
    ESP.getChipId(),
    ESP.getChipId(),
    mqttAvailTopic,
    mqttCmdTopic,
    mqttStateTopic);
  
  mqtt.publish(mqttDiscTopic, discoveryMsg);
  
  mqtt.publish(mqttAvailTopic, "online");
  
  if (!motorStartMS)
    mqtt.publish(mqttStateTopic, CurrentlyOpen ? "open" : "closed");
  
  lastDiscovery = millis();
}

void HassCommand(char *topic, byte *payload, unsigned int len)
{
  if (!payload || !len || toupper(payload[0]) == 'S')
    stopAll();
  else if (toupper(payload[0]) == 'O')
    openWindow();
  else if (toupper(payload[0]) == 'C')
    closeWindow();
    
  Serial.print(F("Command was from MQTT: "));
  Serial.println(payload ? payload[0] : 0);
}
