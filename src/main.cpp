#include <Arduino.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>
#include <U8g2lib.h>

Preferences preferences;
WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer server(80);
WiFiManager wifiManager;

WiFiManagerParameter *mqttServerField;
WiFiManagerParameter *mqttPortField;
WiFiManagerParameter *mqttNodeField;
WiFiManagerParameter *mqttPrefixField;

U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

const char APP_NAME[] = "doorbell";

const int RELAY_PIN = 16;
const int BUTTON_PIN = 17;

const int WATCHDOG_TIMEOUT = 5;

unsigned long lastActivateChimeTime = 0;
unsigned long lastActivateStatusTime = 0;
unsigned long lastStatusCheckTime = 0;
unsigned long lastRenderTime = 0;
unsigned long lastScreensaveTime = 0;

const char SPINNER[] = "<<<<<";
unsigned int spinnerIdx = 0;

// MQTT server config.
char defaultMqttServer[40] = "";
char defaultMqttPort[6] = "1883";
char defaultMqttNodeName[40] = "doorbell";
char defaultMqttPrefix[40] = "home/frontdoor";
String mqttServer;
String mqttPort;
String mqttNodeName;
String mqttPrefix;
char mqttTopic[128];

bool shouldSaveConfig = false;

void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered WiFi config mode");
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

void saveConfigCallback () {
  Serial.println("setting shouldSaveConfig to true");
  shouldSaveConfig = true;
}

void webHandleNotFound() {
  server.send(404, "application/json", "{\"message\":\"Not found\"}");
}

// TODO: include MQTT client connection status in JSON.
void webHandleStatus() {
  lastActivateStatusTime = millis();

  String json;
  json.reserve(1024);
  json += "{\"uptime\": ";
  json += millis();
  json += ", \"heap_free\": ";
  json += ESP.getFreeHeap();

  json += ", \"mqtt_config\": {";
  json += "\"server\": \"";
  json += mqttServer;
  json += "\", \"port\": ";
  json += mqttPort;
  json += ", \"node\": \"";
  json += mqttNodeName;
  json += "\", \"prefix\": \"";
  json += mqttPrefix;
  json += "\"}";

  json += "]}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// Forget all saved preferences (WiFi & MQTT) and reboot.
void webHandleReset() {
  Serial.println("Resetting.");
  server.send(202);

  // Clear the custom config vars.
  preferences.begin(APP_NAME, false);
  preferences.clear();
  preferences.end();

  // Clear the WiFi connection credentials.
  wifiManager.resetSettings();

  ESP.restart();
}

void ringDoorbell() {
  digitalWrite(RELAY_PIN, HIGH);
  mqttClient.publish(mqttTopic, "pressed");
  lastActivateChimeTime = millis();
}

// Ring the doorbell!
void webHandleRing() {
  Serial.println("Ringing the doorbell.");
  server.send(202);
  ringDoorbell();
}

// Simply reboot the system.
void webHandleReboot() {
  Serial.println("Rebooting.");
  server.send(202);
  ESP.restart();
}

// https://github.com/knolleary/pubsubclient/blob/master/examples/mqtt_esp8266/mqtt_esp8266.ino
void mqttReconnect() {
  if(mqttClient.connected()) {
    return;
  }

  if(WiFi.status() != WL_CONNECTED){
    return;
  }

  static unsigned long lastMqttReconnectMillis = 0;
  if(millis() - lastMqttReconnectMillis < 2000) {
    return;
  }
  lastMqttReconnectMillis = millis();

  Serial.print("Attempting MQTT connection to ");
  Serial.print(mqttServer);
  Serial.print(":");
  Serial.println(mqttPort);
  mqttClient.setServer(mqttServer.c_str(), mqttPort.toInt());

  if (mqttClient.connect(mqttNodeName.c_str())) {
    Serial.println("MQTT connected");
  } else {
    Serial.print("MQTT connection failed, rc=");
    Serial.println(mqttClient.state());
  }
}

void setup() {
  WiFi.mode(WIFI_STA);
  preferences.begin(APP_NAME, false);
  Serial.begin(9600);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT);

  // Allow the user to configure MQTT params on the same UI as the WiFi.
  mqttServerField = new WiFiManagerParameter("server", "mqtt server", defaultMqttServer, 40);
  mqttPortField = new WiFiManagerParameter("port", "mqtt port", defaultMqttPort, 6);
  mqttNodeField = new WiFiManagerParameter("nodename", "mqtt node name", defaultMqttNodeName, 40);
  mqttPrefixField = new WiFiManagerParameter("prefix", "mqtt prefix", defaultMqttPrefix, 40);
  wifiManager.addParameter(mqttServerField);
  wifiManager.addParameter(mqttPortField);
  wifiManager.addParameter(mqttNodeField);
  wifiManager.addParameter(mqttPrefixField);

  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setConfigPortalTimeout(60);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setWiFiAutoReconnect(true);

  if(wifiManager.autoConnect("DoorbellAP")){
    Serial.println("WiFi connected");
  }
  else {
    Serial.println("WiFi not connected, config portal running");
  }

  // load MQTT config vars from flash storage.
  Serial.println("Reading MQTT config vars");
  mqttServer = preferences.getString("mqttServer", defaultMqttServer);
  mqttPort = preferences.getString("mqttPort", defaultMqttPort).toInt();
  mqttNodeName = preferences.getString("mqttNodeName", defaultMqttNodeName);
  mqttPrefix = preferences.getString("mqttPrefix", defaultMqttPrefix);
  sprintf(mqttTopic, "%s/%s", mqttPrefix.c_str(), mqttNodeName.c_str());
  Serial.println("using MQTT prefs...");
  Serial.println(mqttServer);
  Serial.println(mqttPort);
  Serial.println(mqttNodeName);
  Serial.println(mqttPrefix);
  Serial.println(mqttTopic);

  server.on("/status", HTTP_GET, webHandleStatus);
  server.on("/reboot", HTTP_PUT, webHandleReboot);
  server.on("/reset", HTTP_PUT, webHandleReset);
  server.on("/ring", HTTP_PUT, webHandleRing);
  server.onNotFound(webHandleNotFound);
  server.begin();
  Serial.println("HTTP server started");

  Serial.println("Configuring WDT watchdog...");
  esp_task_wdt_init(WATCHDOG_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  display.begin();
}

void saveConfig() {
  Serial.println("Writing MQTT config vars");
  char server[40];
  char port[6];
  char node[40];
  char prefix[40];
  strcpy(server, mqttServerField->getValue());
  strcpy(port, mqttPortField->getValue());
  strcpy(node, mqttNodeField->getValue());
  strcpy(prefix, mqttPrefixField->getValue());
  // Persist the user-input MQTT params to flash storage.
  preferences.putString("mqttServer", server);
  preferences.putString("mqttPort", port);
  preferences.putString("mqttNodeName", node);
  preferences.putString("mqttPrefix", prefix);
  shouldSaveConfig = false;
  preferences.end();
  ESP.restart();
}

// Display that the doorbell is ringing.
void displayChimeLoop() {
  if(millis()-lastRenderTime < 200) return;
  lastRenderTime = millis();

  display.setPowerSave(0);
  display.firstPage();
  do {
    display.setFont(u8g2_font_helvB14_tr);
    display.drawStr(38,40, "RING!");
  } while ( display.nextPage() );
}

void displayStatusLoop() {
  display.setPowerSave(0);

  if(millis()-lastRenderTime < 1000) return;
  lastRenderTime = millis();

  char spinner[1] = "";
  char status1[128] = "";
  char status2[128] = "";
  char status3[128] = "";

  if(WiFi.status() == WL_CONNECTED) {
    sprintf_P(status1, "%s %i", WiFi.SSID(), WiFi.RSSI());
    sprintf_P(status2, "%s", WiFi.localIP().toString());
  } else {
    strcpy(status1, "No WiFi!");
    strcpy(status2, "");
  }

  if (mqttClient.connected()) {
    sprintf_P(status3, "%s:%i", mqttServer.c_str(), mqttPort.toInt());
  } else {
    sprintf_P(status3, "No MQTT! (%s:%i)", mqttServer.c_str(), mqttPort.toInt());
  }

  if(spinnerIdx > 4) spinnerIdx = 0;
  strcpy(spinner, &SPINNER[spinnerIdx]);
  spinnerIdx += 1;

  display.firstPage();
  do {
    display.setFont(u8g2_font_helvB10_tr);
    display.drawStr(0,16, "DOORBELL");
    display.drawStr(90,16, spinner);
    display.drawStr(0,32, status1);
    display.setFont(u8g2_font_helvR08_tr);
    display.drawStr(0,48, status2);
    display.drawStr(0,64, status3);
  } while ( display.nextPage() );
}

void displayLoop() {
  // Doorbell is ringing, display the chime screen.
  if(lastActivateChimeTime > 0 && millis()-lastActivateChimeTime < 10000) {
    displayChimeLoop();
    return;
  }

  // Show the status screen.
  if(lastActivateStatusTime > 0 && millis()-lastActivateStatusTime < 10000) {
    displayStatusLoop();
    return;
  }

  if(millis()-lastRenderTime < 100) return;

  display.setPowerSave(1);
}

void loop() {
  esp_task_wdt_reset();

  displayLoop();

  if(shouldSaveConfig) {
    saveConfig();
  }

  wifiManager.process();

  // Check WiFi status every few seconds and reconnect if not connected.
  if(millis()-lastStatusCheckTime > 2000 ){
    if(WiFi.status() != WL_CONNECTED){
      Serial.println("No WiFi, reconnecting");
      WiFi.reconnect();
    }
    lastStatusCheckTime = millis();
  }

  if (mqttClient.connected()) {
    mqttClient.loop();
  } else {
    mqttReconnect();
  }

  server.handleClient();

  // Deactivate the doorbell chime after a delay.
  if(millis() - lastActivateChimeTime > 3000) {
    if (digitalRead(RELAY_PIN) == HIGH) {
      digitalWrite(RELAY_PIN, LOW);
    }
  }

  // Detect button push.
  if (digitalRead(BUTTON_PIN) == HIGH) {
    ringDoorbell();
  }
}
