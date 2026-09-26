// =================================================================
// ESP32 MQTT Client - HA Architecture with Dual Broker Failover
// =================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>
#include <Ticker.h>

// --- Configuration ---
const char* mqtt_broker_1 = "mosquitto-muthosech.espserver.site";
const char* mqtt_broker_2 = "mosquitto-muthosech.espserver.site"; // Secondary broker for failover
const uint16_t mqtt_port = 1883; // Use 8883 for TLS in production
const char* mqtt_user = ""; // Set your MQTT username if required
const char* mqtt_pass = ""; // Set your MQTT password if required

#define WDT_TIMEOUT 30

// Firmware Update URLs
const char* firmwareUrl = "https://github.com/shohidmax/pumpv3/releases/download/shohidpump/abbu_pump_online.ino.bin";
const char* versionUrl = "https://raw.githubusercontent.com/shohidmax/pumpv3/refs/heads/main/version.txt";
const char* currentFirmwareVersion = "1.0.0";

// Timers
unsigned long lastUpdateCheck = 0;
const unsigned long updateCheckInterval = 5 * 60 * 1000;
unsigned long lastWifiCheck = 0;
const unsigned long wifiCheckInterval = 10000;

// --- PIN DEFINITIONS ---
#define RELAY_1 12
#define RELAY_2 14
#define relay_3 13
#define SWITCH_1 23
#define SWITCH_2 22
#define LED_PIN 2

// --- GLOBAL VARIABLES ---
WiFiClient espClient;
PubSubClient mqttClient(espClient);

String macAddress;
String topicStatus;
String topicCommand;

int primaryFailCount = 0;
bool useSecondaryBroker = false;

unsigned long relay1_timer = 0;
unsigned long relay2_timer = 0;
unsigned long relay3_timer = 0;
const int relay_duration = 1000; // 1 Second Pulse

unsigned long lastStatusUpdate = 0;
String lastMotorStat = "";
String lastSysMode = "";
int lastWifiSignal = 0;

unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 200;
String stableMotorState = "OFF";

Ticker blinker;

// --- FORWARD DECLARATIONS ---
void checkForFirmwareUpdate();
String fetchLatestVersion();
void downloadAndApplyFirmware();
bool startOTAUpdate(WiFiClient* client, int contentLength);
void reconnectMQTT();

// --- FUNCTIONS ---

void tick() {
  int state = digitalRead(LED_PIN);
  digitalWrite(LED_PIN, !state);
}

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  blinker.attach(0.3, tick);
}

void sendStatus() {
    String reading = (digitalRead(SWITCH_1) == LOW) ? "ON" : "OFF";
    String currentMode = (digitalRead(SWITCH_2) == LOW) ? "Normal" : "Emergency";
    int currentSignal = constrain(map(WiFi.RSSI(), -100, -30, 0, 100), 0, 100);

    if (reading != stableMotorState) {
       if ((millis() - lastDebounceTime) > debounceDelay) {
         stableMotorState = reading;
         lastDebounceTime = millis();
       }
    } else {
       lastDebounceTime = millis();
    }
    
    String currentMotor = stableMotorState;

    if (currentMotor != lastMotorStat || currentMode != lastSysMode || abs(currentSignal - lastWifiSignal) > 5 || millis() - lastStatusUpdate > 5000) {
        
        JsonDocument doc;
        doc["type"] = "statusUpdate";
        
        JsonObject payload = doc.createNestedObject("payload");
        payload["motorStatus"] = currentMotor;
        payload["systemMode"] = currentMode;
        payload["wifiSignal"] = currentSignal;
        payload["localIP"] = WiFi.localIP().toString();
        payload["version"] = currentFirmwareVersion;
        
        String jsonString;
        serializeJson(doc, jsonString);
        
        if (mqttClient.connected()) {
            mqttClient.publish(topicStatus.c_str(), jsonString.c_str(), true); // Retained message
        }

        lastMotorStat = currentMotor;
        lastSysMode = currentMode;
        lastWifiSignal = currentSignal;
        lastStatusUpdate = millis();
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    esp_task_wdt_reset();
    String message = "";
    for (int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    
    Serial.printf("Message arrived on topic: %s\n", topic);
    
    if (String(topic) == topicCommand) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, message);
        if (!error) {
            String command = doc["command"];
            if (command == "RELAY_1") {
                digitalWrite(RELAY_1, HIGH);
                relay1_timer = millis();
            } else if (command == "RELAY_2") {
                digitalWrite(RELAY_2, HIGH);
                relay2_timer = millis();
            } else if (command == "RESET") {
                digitalWrite(relay_3, HIGH);
                relay3_timer = millis();
            } else if (command == "LED_ON") {
                digitalWrite(LED_PIN, HIGH);
                Serial.println("Built-in LED turned ON remotely");
            } else if (command == "LED_OFF") {
                digitalWrite(LED_PIN, LOW);
                Serial.println("Built-in LED turned OFF remotely");
            } else if (command == "RESTART_ESP") {
                delay(500);
                ESP.restart();
            } else if (command == "CHECK_UPDATE") {
                checkForFirmwareUpdate();
            }
            lastStatusUpdate = 0; // Force immediate update
        }
    }
}

unsigned long lastReconnectAttempt = 0;

void reconnectMQTT() {
    if (!mqttClient.connected()) {
        if (lastReconnectAttempt > 0 && millis() - lastReconnectAttempt < 5000) {
            return; // Wait 5 seconds before retrying
        }
        lastReconnectAttempt = millis();

        const char* broker = useSecondaryBroker ? mqtt_broker_2 : mqtt_broker_1;
        mqttClient.setServer(broker, mqtt_port);
        
        Serial.print("Attempting MQTT connection to ");
        Serial.print(broker);
        Serial.print("...");
        
        if (mqttClient.connect(macAddress.c_str(), mqtt_user, mqtt_pass)) {
            Serial.println("connected");
            primaryFailCount = 0; // Reset fail count on success
            
            // Subscribe to Command Topic
            mqttClient.subscribe(topicCommand.c_str());
            
            // Send Initial Status
            lastStatusUpdate = 0; 
            sendStatus();
        } else {
            Serial.print("failed, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" try again in 5 seconds");
            
            if (!useSecondaryBroker) {
                primaryFailCount++;
                if (primaryFailCount >= 3) {
                    Serial.println("Primary broker failed 3 times. Switching to Secondary Broker!");
                    useSecondaryBroker = true;
                }
            } else {
                // If secondary fails, maybe fallback to primary after a while, or restart
                Serial.println("Secondary broker failed. Rebooting network stack soon.");
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    pinMode(RELAY_1, OUTPUT); digitalWrite(RELAY_1, LOW);
    pinMode(RELAY_2, OUTPUT); digitalWrite(RELAY_2, LOW);
    pinMode(relay_3, OUTPUT); digitalWrite(relay_3, LOW);
    pinMode(SWITCH_1, INPUT_PULLUP);
    pinMode(SWITCH_2, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);

    esp_task_wdt_deinit();
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_config);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    
    WiFiManager wm;
    wm.setAPCallback(configModeCallback);
    wm.setConfigPortalTimeout(180);

    if (!wm.autoConnect("Mutho-Sech")) {
        Serial.println("Failed to connect. Restarting...");
        delay(3000);
        ESP.restart();
    }

    blinker.detach();
    digitalWrite(LED_PIN, HIGH);

    macAddress = WiFi.macAddress();
    macAddress.replace(":", "");
    topicStatus = "device/" + macAddress + "/status";
    topicCommand = "device/" + macAddress + "/command";
    
    Serial.println("WiFi Connected!");
    Serial.println("MAC: " + macAddress);
    
    esp_task_wdt_add(NULL);
    checkForFirmwareUpdate();
    
    mqttClient.setCallback(mqttCallback);
}

void loop() {
    esp_task_wdt_reset();
    
    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - lastWifiCheck > wifiCheckInterval) {
            lastWifiCheck = millis();
            Serial.println("WiFi Lost! Attempting reconnect...");
            WiFi.reconnect();
        }
    } else {
        if (!mqttClient.connected()) {
            reconnectMQTT();
        }
        mqttClient.loop();
    }

    // Handle Relay Timers
    unsigned long currentMillis = millis();
    if (relay1_timer > 0 && currentMillis - relay1_timer >= relay_duration) {
        digitalWrite(RELAY_1, LOW); relay1_timer = 0;
    }
    if (relay2_timer > 0 && currentMillis - relay2_timer >= relay_duration) {
        digitalWrite(RELAY_2, LOW); relay2_timer = 0;
    }
    if (relay3_timer > 0 && currentMillis - relay3_timer >= relay_duration) {
        digitalWrite(relay_3, LOW); relay3_timer = 0;
    }

    // Check Status and Send Update
    if (mqttClient.connected()) {
        sendStatus();
    }
}

// --- OTA FUNCTIONS ---
bool isNewerVersion(String current, String latest) {
    int c_major = 0, c_minor = 0, c_patch = 0;
    int l_major = 0, l_minor = 0, l_patch = 0;
    sscanf(current.c_str(), "%d.%d.%d", &c_major, &c_minor, &c_patch);
    sscanf(latest.c_str(), "%d.%d.%d", &l_major, &l_minor, &l_patch);
    if (l_major > c_major) return true;
    if (l_major < c_major) return false;
    if (l_minor > c_minor) return true;
    if (l_minor < c_minor) return false;
    if (l_patch > c_patch) return true;
    return false;
}

void checkForFirmwareUpdate() {
  if (WiFi.status() != WL_CONNECTED) return;
  String latestVersion = fetchLatestVersion();
  while (latestVersion.endsWith(".")) latestVersion.remove(latestVersion.length()-1);
  if (latestVersion == "") return;
  if (isNewerVersion(String(currentFirmwareVersion), latestVersion)) {
    esp_task_wdt_reset();
    downloadAndApplyFirmware();
  }
}

String fetchLatestVersion() {
  esp_task_wdt_reset();
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, versionUrl);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String latestVersion = http.getString();
    latestVersion.trim();
    http.end();
    return latestVersion;
  }
  http.end();
  return "";
}

void downloadAndApplyFirmware() {
  esp_task_wdt_reset();
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000); 
  http.begin(client, firmwareUrl);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    int contentLength = http.getSize();
    if (contentLength > 0) {
      WiFiClient* stream = http.getStreamPtr();
      if (startOTAUpdate(stream, contentLength)) {
        delay(1000);
        ESP.restart();
      }
    }
  }
  http.end();
}

bool startOTAUpdate(WiFiClient* client, int contentLength) {
  if (!Update.begin(contentLength)) return false;
  size_t written = 0;
  const unsigned long timeoutDuration = 120 * 1000;
  unsigned long lastDataTime = millis();

  while (written < contentLength) {
    esp_task_wdt_reset();
    if (client->available()) {
      uint8_t buffer[256];
      size_t len = client->read(buffer, sizeof(buffer));
      if (len > 0) {
        Update.write(buffer, len);
        written += len;
        lastDataTime = millis();
      }
    }
    if (millis() - lastDataTime > timeoutDuration) {
      Update.abort();
      return false;
    }
    yield();
  }
  if (written != contentLength) {
    Update.abort();
    return false;
  }
  return Update.end();
}