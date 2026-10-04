// =================================================================
// ESP32 MQTT Client - WiFi + SIM800L GPRS Auto Failover + IVR Voice Call
// =================================================================

// Define this before including TinyGsmClient.h
#define TINY_GSM_MODEM_SIM800
#define TINY_GSM_DEBUG Serial

#include <TinyGsmClient.h>
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
#include <DFRobotDFPlayerMini.h>

// --- Configuration ---
const char* mqtt_broker_1 = "mosquitto-muthosech.espserver.site";
const char* mqtt_broker_2 = "mosquitto-muthosech.espserver.site"; // Secondary broker for failover
const uint16_t mqtt_port = 1883; 
const char* mqtt_user = ""; 
const char* mqtt_pass = ""; 

// --- GPRS (SIM Internet) Configuration ---
const char apn[]      = "gpinternet"; 
const char gprsUser[] = "";
const char gprsPass[] = "";

#define WDT_TIMEOUT 120

// Firmware Update URLs
const char* firmwareUrl = "https://github.com/shohidmax/pumpv3/releases/download/shohidpump/abbu_pump_online.ino.bin";
const char* versionUrl = "https://raw.githubusercontent.com/shohidmax/pumpv3/refs/heads/main/version.txt";
const char* currentFirmwareVersion = "2.3.0"; // Bump version for IVR Integration

// Timers
unsigned long lastUpdateCheck = 0;
const unsigned long updateCheckInterval = 5 * 60 * 1000;
unsigned long lastWifiCheck = 0;
const unsigned long wifiCheckInterval = 10000;

// --- PIN DEFINITIONS ---
#define RELAY_1 25
#define RELAY_2 26
#define relay_3 32 
#define SWITCH_1 23
#define SWITCH_2 22
#define LED_PIN 2

// --- SIM800L & DFPLAYER PINS ---
#define ESP_RX_FROM_SIM800 16 
#define ESP_TX_TO_SIM800 17   
#define SIM800_RST 33
#define ESP_RX_FROM_DFPLAYER 14 
#define ESP_TX_TO_DFPLAYER 27   

// --- RELAY POLARITY CONFIGURATION (Active-HIGH) ---
#define RELAY_TRIGGER HIGH
#define RELAY_RELEASE LOW

#define USE_PHYSICAL_SWITCH false

// --- SPY STREAM FOR DTMF INTERCEPTION ---
// This safely reads the SIM800L serial data to catch incoming calls and DTMF 
// without stealing the internet data from TinyGSM.
class SpyStream : public Stream {
public:
  HardwareSerial* target;
  String buffer;
  bool dtmf1 = false;
  bool dtmf2 = false;
  bool dtmf0 = false;
  bool ring = false;
  bool noCarrier = false;
  
  SpyStream(HardwareSerial* t) : target(t) { buffer.reserve(64); }
  
  int available() override { return target->available(); }
  
  int read() override { 
    int c = target->read(); 
    if (c >= 0) {
      char ch = (char)c;
      buffer += ch;
      if (ch == '\n') {
        if (buffer.indexOf("+DTMF: 1") != -1) dtmf1 = true;
        else if (buffer.indexOf("+DTMF: 2") != -1) dtmf2 = true;
        else if (buffer.indexOf("+DTMF: 0") != -1) dtmf0 = true;
        else if (buffer.indexOf("RING") != -1) ring = true;
        else if (buffer.indexOf("NO CARRIER") != -1) noCarrier = true;
        buffer = "";
      }
      if (buffer.length() > 60) buffer = ""; // Prevent memory overflow
    }
    return c;
  }
  
  int peek() override { return target->peek(); }
  void flush() override { target->flush(); }
  size_t write(uint8_t c) override { return target->write(c); }
  size_t write(const uint8_t *buf, size_t size) override { return target->write(buf, size); }
};

// --- GLOBAL VARIABLES ---
WiFiClient espClient;
SpyStream spySerial(&Serial1);
TinyGsm modem(spySerial);
TinyGsmClient gsmClient(modem);
PubSubClient mqttClient;
DFRobotDFPlayerMini myDFPlayer;

String macAddress;
String topicStatus;
String topicCommand;

int primaryFailCount = 0;
bool useSecondaryBroker = false;
bool isUsingGPRS = false; 
bool gprsConnected = false; // Tracks actual PDP context status
bool inCall = false;
unsigned long callStartTime = 0;

unsigned long relay1_timer = 0;
unsigned long relay2_timer = 0;
unsigned long relay3_timer = 0;
const int relay_duration = 1000; 

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
void sendStatus();

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
    String reading;
    if (USE_PHYSICAL_SWITCH) {
        reading = (digitalRead(SWITCH_1) == LOW) ? "ON" : "OFF";
    } else {
        reading = stableMotorState;
    }
    String currentMode = (digitalRead(SWITCH_2) == LOW) ? "Normal" : "Emergency";
    
    int currentSignal = 0;
    if (isUsingGPRS) {
        int csq = modem.getSignalQuality(); 
        currentSignal = constrain(map(csq, 0, 31, 0, 100), 0, 100);
    } else {
        currentSignal = constrain(map(WiFi.RSSI(), -100, -30, 0, 100), 0, 100);
    }

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
        payload["localIP"] = isUsingGPRS ? modem.getLocalIP() : WiFi.localIP().toString();
        payload["version"] = currentFirmwareVersion;
        payload["network"] = isUsingGPRS ? "SIM800L (GPRS)" : "WiFi";
        
        String jsonString;
        serializeJson(doc, jsonString);
        
        if (mqttClient.connected()) {
            mqttClient.publish(topicStatus.c_str(), jsonString.c_str(), true); 
        }

        lastMotorStat = currentMotor;
        lastSysMode = currentMode;
        lastWifiSignal = currentSignal;
        lastStatusUpdate = millis();
    }
}

void processCommand(String command) {
    if (command == "RELAY_1" || command == "ON" || command == "START") {
        digitalWrite(RELAY_1, RELAY_TRIGGER);
        relay1_timer = millis();
        stableMotorState = "ON";
        Serial.println("[ACTION] RELAY_1 (START) Pulse Active -> HIGH for 1 sec");
    } else if (command == "RELAY_2" || command == "OFF" || command == "STOP") {
        digitalWrite(RELAY_2, RELAY_TRIGGER);
        relay2_timer = millis();
        stableMotorState = "OFF";
        Serial.println("[ACTION] RELAY_2 (STOP) Pulse Active -> HIGH for 1 sec");
    } else if (command == "RESET") {
        digitalWrite(relay_3, RELAY_TRIGGER);
        relay3_timer = millis();
        Serial.println("[ACTION] RELAY_3 (RESET) Active -> HIGH for 1 sec");
    } else if (command == "LED_ON") {
        digitalWrite(LED_PIN, HIGH);
        Serial.println("[ACTION] Built-in LED turned ON remotely");
    } else if (command == "LED_OFF") {
        digitalWrite(LED_PIN, LOW);
        Serial.println("[ACTION] Built-in LED turned OFF remotely");
    } else if (command == "RESTART_ESP") {
        Serial.println("[ACTION] Restarting ESP32...");
        delay(500);
        ESP.restart();
    } else if (command == "CHECK_UPDATE") {
        Serial.println("[ACTION] Checking for firmware updates...");
        if (isUsingGPRS) {
            Serial.println("[WARN] OTA Updates are disabled over GPRS to prevent failure. Connect to WiFi first.");
        } else {
            checkForFirmwareUpdate();
        }
    }
    lastStatusUpdate = 0; 
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    esp_task_wdt_reset();
    String message = "";
    for (int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    
    Serial.printf("[MQTT] Message on %s: %s\n", topic, message.c_str());
    
    if (String(topic) == topicCommand) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, message);
        if (!error) {
            String command = doc["command"].as<String>();
            Serial.printf("[COMMAND] Processing: %s\n", command.c_str());
            processCommand(command);
        }
    }
}

unsigned long lastReconnectAttempt = 0;
unsigned long lastGprsConnectAttempt = 0;

void reconnectMQTT() {
    if (!mqttClient.connected()) {
        if (lastReconnectAttempt > 0 && millis() - lastReconnectAttempt < 5000) {
            return; 
        }
        lastReconnectAttempt = millis();

        const char* broker = useSecondaryBroker ? mqtt_broker_2 : mqtt_broker_1;
        mqttClient.setServer(broker, mqtt_port);
        
        Serial.print("Attempting MQTT connection to ");
        Serial.print(broker);
        Serial.print(" via ");
        Serial.print(isUsingGPRS ? "GPRS (SIM)" : "WiFi");
        Serial.print("...");
        esp_task_wdt_reset(); // Reset WDT before potentially blocking connection
        
        if (mqttClient.connect(macAddress.c_str(), mqtt_user, mqtt_pass)) {
            Serial.println("connected");
            primaryFailCount = 0; 
            
            bool subOk = mqttClient.subscribe(topicCommand.c_str(), 1);
            Serial.printf("[MQTT] Subscribed to %s -> %s\n", topicCommand.c_str(), subOk ? "SUCCESS" : "FAILED");
            
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
                    primaryFailCount = 0; // Reset counter for secondary
                }
            } else {
                primaryFailCount++;
                Serial.println("Secondary broker failed.");
                if (primaryFailCount >= 3) {
                    Serial.println("Secondary broker failed 3 times. Switching back to Primary Broker!");
                    useSecondaryBroker = false;
                    primaryFailCount = 0;
                }
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    pinMode(RELAY_1, OUTPUT); digitalWrite(RELAY_1, RELAY_RELEASE);
    pinMode(RELAY_2, OUTPUT); digitalWrite(RELAY_2, RELAY_RELEASE);
    pinMode(relay_3, OUTPUT); digitalWrite(relay_3, RELAY_RELEASE);
    pinMode(SWITCH_1, INPUT_PULLUP);
    pinMode(SWITCH_2, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);

    // Initialize SIM800L on Serial1
    pinMode(SIM800_RST, OUTPUT);
    digitalWrite(SIM800_RST, HIGH);
    Serial1.begin(9600, SERIAL_8N1, ESP_RX_FROM_SIM800, ESP_TX_TO_SIM800);
    Serial.println("SIM800L Serial1 Initialized");

    // Initialize DFPlayer on Serial2
    Serial2.begin(9600, SERIAL_8N1, ESP_RX_FROM_DFPLAYER, ESP_TX_TO_DFPLAYER);
    if (!myDFPlayer.begin(Serial2)) {
        Serial.println(F("Unable to begin DFPlayer. Check connection/SD card."));
    } else {
        Serial.println(F("DFPlayer Mini Initialized."));
        myDFPlayer.volume(25); // Volume (0~30)
    }

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
    wm.setConfigPortalTimeout(60); 

    Serial.println("Attempting WiFi Connection...");
    if (!wm.autoConnect("Mutho-Sech")) {
        Serial.println("WiFi Failed to connect. Will fallback to SIM800L GPRS!");
    } else {
        Serial.println("WiFi Connected!");
    }

    blinker.detach();
    digitalWrite(LED_PIN, HIGH);

    macAddress = WiFi.macAddress();
    if(macAddress == "" || macAddress == "00:00:00:00:00:00") {
        uint64_t chipid = ESP.getEfuseMac(); 
        char macStr[13];
        snprintf(macStr, 13, "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
        macAddress = String(macStr);
    } else {
        macAddress.replace(":", "");
    }

    topicStatus = "device/" + macAddress + "/status";
    topicCommand = "device/" + macAddress + "/command";
    
    Serial.println("MAC: " + macAddress);
    
    esp_task_wdt_add(NULL);
    
    mqttClient.setBufferSize(512);
    mqttClient.setKeepAlive(10);
    mqttClient.setSocketTimeout(15);
    mqttClient.setCallback(mqttCallback);

    // Initial Network Selection
    if (WiFi.status() == WL_CONNECTED) {
        mqttClient.setClient(espClient);
        isUsingGPRS = false;
        checkForFirmwareUpdate();
    } else {
        mqttClient.setClient(gsmClient);
        isUsingGPRS = true;
    }
}

void loop() {
    esp_task_wdt_reset();
    
    // --- PHONE CALL & DTMF (IVR) LOGIC ---
    if (spySerial.ring) {
        spySerial.ring = false;
        inCall = true;
        callStartTime = millis();
        Serial.println("Incoming Call! Answering...");
        modem.callAnswer();
        
        // Wait for call to connect, then enable DTMF detection
        delay(1000);
        modem.sendAT("+DDET=1");
        modem.waitResponse();

        // Play welcome audio (Ensure 0001.mp3 is on SD card)
        myDFPlayer.play(1); 
        Serial.println("Played Welcome Audio");
    }
    
    if (spySerial.dtmf1) {
        spySerial.dtmf1 = false;
        Serial.println("DTMF 1 Received (Via Phone): START MOTOR");
        processCommand("START");
        myDFPlayer.play(2); // Play "Motor Started" audio (0002.mp3)
    }
    
    if (spySerial.dtmf2) {
        spySerial.dtmf2 = false;
        Serial.println("DTMF 2 Received (Via Phone): STOP MOTOR");
        processCommand("STOP");
        myDFPlayer.play(3); // Play "Motor Stopped" audio (0003.mp3)
    }
    
    if (spySerial.dtmf0) {
        spySerial.dtmf0 = false;
        inCall = false;
        Serial.println("DTMF 0 Received (Via Phone): HANGING UP");
        modem.callHangup();
    }

    if (spySerial.noCarrier) {
        spySerial.noCarrier = false;
        inCall = false;
        Serial.println("Call Ended (NO CARRIER)");
    }

    // Failsafe to release inCall flag after 60 seconds
    if (inCall && (millis() - callStartTime > 60000)) {
        inCall = false;
        Serial.println("Call Timeout. Releasing call state.");
    }
    
    // --- NETWORK MANAGEMENT (Dual Failover) ---
    if (WiFi.status() == WL_CONNECTED) {
        if (isUsingGPRS) {
            Serial.println("WiFi Restored. Switching from GPRS back to WiFi...");
            isUsingGPRS = false;
            mqttClient.disconnect();
            mqttClient.setClient(espClient);
        }
    } else {
        if (millis() - lastWifiCheck > wifiCheckInterval) {
            lastWifiCheck = millis();
            WiFi.reconnect();
        }
        
        if (!isUsingGPRS) {
            Serial.println("WiFi Lost. Switching to SIM800L GPRS...");
            isUsingGPRS = true;
            gprsConnected = false; // Reset to force GPRS initialization
            mqttClient.disconnect();
            mqttClient.setClient(gsmClient);
        }

        if (isUsingGPRS && (!gprsConnected || !modem.isGprsConnected())) {
            if (millis() - lastGprsConnectAttempt > 10000) { 
                lastGprsConnectAttempt = millis();
                Serial.println("Initializing SIM800L and connecting to GPRS...");
                
                // Only restart modem if we are not in a voice call
                if (!inCall) {
                    if (modem.restart()) {
                        esp_task_wdt_reset(); // Reset WDT after restart
                        if (modem.waitForNetwork(60000)) {
                            esp_task_wdt_reset(); // Reset WDT after network wait
                            if (modem.gprsConnect(apn, gprsUser, gprsPass)) {
                                Serial.println("GPRS Connected Successfully! Waiting 3s...");
                                delay(3000); // Give modem time before TCP attempt
                                gprsConnected = true;
                            } else {
                                Serial.println("GPRS Connect Failed!");
                                gprsConnected = false;
                            }
                        }
                    }
                }
            }
        }
    }

    // --- HANDLE MQTT ---
    if ((!isUsingGPRS && WiFi.status() == WL_CONNECTED) || (isUsingGPRS && gprsConnected)) {
        if (!mqttClient.connected()) {
            reconnectMQTT();
        } else {
            mqttClient.loop();
        }
    }

    // --- HANDLE RELAY TIMERS ---
    unsigned long currentMillis = millis();
    if (relay1_timer > 0 && currentMillis - relay1_timer >= relay_duration) {
        digitalWrite(RELAY_1, RELAY_RELEASE); 
        relay1_timer = 0;
        Serial.println("[ACTION] RELAY_1 (START) Pulse Ended");
    }
    if (relay2_timer > 0 && currentMillis - relay2_timer >= relay_duration) {
        digitalWrite(RELAY_2, RELAY_RELEASE); 
        relay2_timer = 0;
        Serial.println("[ACTION] RELAY_2 (STOP) Pulse Ended");
    }
    if (relay3_timer > 0 && currentMillis - relay3_timer >= relay_duration) {
        digitalWrite(relay_3, RELAY_RELEASE); 
        relay3_timer = 0;
        Serial.println("[ACTION] RELAY_3 (RESET) Pulse Ended");
    }

    // --- OTA CHECK (WiFi ONLY) ---
    if (millis() - lastUpdateCheck > updateCheckInterval) {
        lastUpdateCheck = millis();
        if (!isUsingGPRS) {
            checkForFirmwareUpdate();
        }
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