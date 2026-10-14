// =================================================================
// ESP32 MQTT Client - Pure WiFi Internet + SIM800L Voice Call & IVR DTMF
// (No GPRS / No SIM Internet: SIM800L is 100% Dedicated to Calls & DTMF)
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
#include <DFRobotDFPlayerMini.h>

// --- Configuration: Primary & True Secondary MQTT Brokers (WiFi Only) ---
const char* mqtt_broker_1 = "mosquitto-muthosech.espserver.site"; // Primary Broker
const char* mqtt_broker_2 = "broker.emqx.io";                     // Independent Secondary Broker
const uint16_t mqtt_port = 1883; 
const char* mqtt_user = ""; 
const char* mqtt_pass = ""; 

#define WDT_TIMEOUT 120

// Firmware Update URLs (via WiFi)
const char* firmwareUrl = "https://github.com/shohidmax/pumpv3/releases/download/shohidpump/abbu_pump_online.ino.bin";
const char* versionUrl = "https://raw.githubusercontent.com/shohidmax/pumpv3/refs/heads/main/version.txt";
const char* currentFirmwareVersion = "2.4.0"; 

// --- SECURITY & CALLER ID WHITELIST ---
// Set enableCallerIdWhitelist to true in production to restrict motor call control
bool enableCallerIdWhitelist = false; 
const char* authorizedNumbers[] = {
    "+8801793496030", // Master phone number
    "+8801800000000"  // Secondary phone number
};
const size_t authorizedNumberCount = sizeof(authorizedNumbers) / sizeof(authorizedNumbers[0]);

// Timers
unsigned long lastUpdateCheck = 0;
const unsigned long updateCheckInterval = 5 * 60 * 1000;
unsigned long lastWifiCheck = 0;
const unsigned long wifiCheckInterval = 10000;

// --- PIN DEFINITIONS ---
#define RELAY_1 25   // START Pulse Output
#define RELAY_2 26   // STOP Pulse Output
#define relay_3 32   // RESET Pulse Output
#define SWITCH_1 23  // Motor Running Feedback Input (Contactor Aux NO -> GND)
#define SWITCH_2 22  // Emergency Stop Switch Input (LOW = Normal, HIGH = Emergency)
#define LED_PIN 2    // Built-in Status LED

// --- SIM800L & DFPLAYER PINS ---
#define ESP_RX_FROM_SIM800 16 
#define ESP_TX_TO_SIM800 17   
#define SIM800_RST 33
#define ESP_RX_FROM_DFPLAYER 14 
#define ESP_TX_TO_DFPLAYER 27   

// --- RELAY POLARITY CONFIGURATION (Active-HIGH) ---
#define RELAY_TRIGGER HIGH
#define RELAY_RELEASE LOW

// --- MOTOR FEEDBACK CONFIGURATION ---
// When true, actual motor state is strictly validated via physical feedback pin (SWITCH_1)
#define USE_PHYSICAL_FEEDBACK true
#define FEEDBACK_ACTIVE_LEVEL LOW // LOW means contactor pulled in (motor running)

// --- SIM800L CALL & DTMF STATE (PURE SERIAL / NO GPRS) ---
String simLineBuffer = "";
bool simRingDetected = false;
bool simNoCarrier = false;
String simCallerNumber = "";
char simLastDtmf = 0;
bool simHasDtmf = false;

// --- NON-BLOCKING CALL STATE MACHINE ---
enum CallState {
  CALL_IDLE,
  CALL_RINGING,
  CALL_ANSWERING,
  CALL_CONNECTED,
  CALL_HANGING_UP
};
CallState currentCallState = CALL_IDLE;
unsigned long callTimer = 0;
String activeCaller = "";

// --- GLOBAL CLIENTS ---
WiFiClient espClient;
PubSubClient mqttClient(espClient);
DFRobotDFPlayerMini myDFPlayer;

String macAddress;
String topicStatus;
String topicCommand;

int primaryFailCount = 0;
bool useSecondaryBroker = false;

unsigned long relay1_timer = 0;
unsigned long relay2_timer = 0;
unsigned long relay3_timer = 0;
const int relay_duration = 1000; 

// Motor Feedback & Safety Variables
String pendingVerificationState = "";
unsigned long verificationStartTime = 0;
String lastActionDescription = "System Boot";

unsigned long lastStatusUpdate = 0;
String lastMotorStat = "";
String lastSysMode = "";
int lastWifiSignal = 0;

unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 200;
String stableMotorState = "OFF";

Ticker blinker;

// --- FORWARD DECLARATIONS ---
void checkForFirmwareUpdate();
String fetchLatestVersion();
void downloadAndApplyFirmware();
bool startOTAUpdate(WiFiClient* client, int contentLength);
void reconnectMQTT();
void sendStatus();
bool getActualMotorRunning();
bool isAuthorizedCaller(const String& number);
void processSimSerial();
void parseSimLine(const String& line);

// --- HELPER FUNCTIONS ---

void tick() {
  int state = digitalRead(LED_PIN);
  digitalWrite(LED_PIN, !state);
}

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  blinker.attach(0.3, tick);
}

// Check real motor physical state via contactor feedback
bool getActualMotorRunning() {
  if (USE_PHYSICAL_FEEDBACK) {
    return (digitalRead(SWITCH_1) == FEEDBACK_ACTIVE_LEVEL);
  } else {
    return (stableMotorState == "ON");
  }
}

// Check whether caller number is authorized in whitelist
bool isAuthorizedCaller(const String& number) {
  if (!enableCallerIdWhitelist) return true; // Whitelist disabled -> allow all
  if (number.length() == 0) return false;     // Unknown caller ID -> reject if whitelist active

  for (size_t i = 0; i < authorizedNumberCount; i++) {
    String auth = authorizedNumbers[i];
    if (number.indexOf(auth) != -1 || auth.indexOf(number) != -1) {
      return true;
    }
  }
  return false;
}

// Send current status via MQTT (over WiFi)
void sendStatus() {
    // Read debounced feedback
    String reading = getActualMotorRunning() ? "ON" : "OFF";
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
        payload["lastAction"] = lastActionDescription;
        payload["wifiSignal"] = currentSignal;
        payload["localIP"] = WiFi.localIP().toString();
        payload["version"] = currentFirmwareVersion;
        payload["network"] = "WiFi";
        payload["gsmMode"] = "Voice Calls Only (No GPRS)";
        
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

// Process control commands with safety interlocks & feedback validation
void processCommand(String command) {
    unsigned long now = millis();
    bool isEmergencyActive = (digitalRead(SWITCH_2) == HIGH);

    if (command == "RELAY_1" || command == "ON" || command == "START") {
        if (isEmergencyActive) {
            Serial.println("[SAFETY] START Rejected: Emergency Stop switch is ACTIVE!");
            lastActionDescription = "START Rejected (Emergency Active)";
            lastStatusUpdate = 0;
            return;
        }
        if (getActualMotorRunning()) {
            Serial.println("[SAFETY] Motor is ALREADY RUNNING. START pulse ignored.");
            lastActionDescription = "START Ignored (Already Running)";
            lastStatusUpdate = 0;
            return;
        }

        // Mutual Exclusion: Ensure STOP relay is strictly inactive
        digitalWrite(RELAY_2, RELAY_RELEASE);
        relay2_timer = 0;

        // Pulse START Relay
        digitalWrite(RELAY_1, RELAY_TRIGGER);
        relay1_timer = now;
        pendingVerificationState = "ON";
        verificationStartTime = now;
        lastActionDescription = "Starting Motor (Pulse Active)";
        Serial.println("[ACTION] RELAY_1 (START) Pulse Active -> HIGH for 1 sec");
    } 
    else if (command == "RELAY_2" || command == "OFF" || command == "STOP") {
        if (!getActualMotorRunning() && stableMotorState == "OFF") {
            Serial.println("[SAFETY] Motor is ALREADY STOPPED. STOP pulse ignored.");
            lastActionDescription = "STOP Ignored (Already Stopped)";
            lastStatusUpdate = 0;
            return;
        }

        // Mutual Exclusion: Ensure START relay is strictly inactive
        digitalWrite(RELAY_1, RELAY_RELEASE);
        relay1_timer = 0;

        // Pulse STOP Relay
        digitalWrite(RELAY_2, RELAY_TRIGGER);
        relay2_timer = now;
        pendingVerificationState = "OFF";
        verificationStartTime = now;
        lastActionDescription = "Stopping Motor (Pulse Active)";
        Serial.println("[ACTION] RELAY_2 (STOP) Pulse Active -> HIGH for 1 sec");
    } 
    else if (command == "RESET") {
        digitalWrite(relay_3, RELAY_TRIGGER);
        relay3_timer = now;
        lastActionDescription = "System Reset Pulse";
        Serial.println("[ACTION] RELAY_3 (RESET) Active -> HIGH for 1 sec");
    } 
    else if (command == "LED_ON") {
        digitalWrite(LED_PIN, HIGH);
        Serial.println("[ACTION] Built-in LED turned ON remotely");
    } 
    else if (command == "LED_OFF") {
        digitalWrite(LED_PIN, LOW);
        Serial.println("[ACTION] Built-in LED turned OFF remotely");
    } 
    else if (command == "RESTART_ESP") {
        Serial.println("[ACTION] Restarting ESP32...");
        delay(500);
        ESP.restart();
    } 
    else if (command == "CHECK_UPDATE") {
        Serial.println("[ACTION] Checking for firmware updates via WiFi...");
        checkForFirmwareUpdate();
    }
    lastStatusUpdate = 0; 
}

// Safe MQTT Callback with payload size validation & strict command whitelist
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    esp_task_wdt_reset();

    // Prevent buffer overflow from oversized payloads
    if (length > 256) {
        Serial.println("[MQTT SECURITY] Payload exceeds max allowed size (256 bytes). Dropping.");
        return;
    }

    String message = "";
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    
    Serial.printf("[MQTT] Message on %s: %s\n", topic, message.c_str());
    
    if (String(topic) == topicCommand) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, message);
        if (!error && doc["command"].is<String>()) {
            String command = doc["command"].as<String>();

            // Strict Command Whitelist Verification
            if (command == "ON" || command == "OFF" || command == "START" || command == "STOP" || 
                command == "RESET" || command == "LED_ON" || command == "LED_OFF" || 
                command == "RESTART_ESP" || command == "CHECK_UPDATE") {
                Serial.printf("[COMMAND] Authorized command processing: %s\n", command.c_str());
                processCommand(command);
            } else {
                Serial.printf("[SECURITY] Rejected unrecognized command: %s\n", command.c_str());
            }
        } else {
            Serial.println("[SECURITY] Malformed JSON payload received on command topic.");
        }
    }
}

unsigned long lastReconnectAttempt = 0;

void reconnectMQTT() {
    if (!mqttClient.connected()) {
        if (lastReconnectAttempt > 0 && millis() - lastReconnectAttempt < 5000) {
            return; 
        }
        lastReconnectAttempt = millis();

        const char* broker = useSecondaryBroker ? mqtt_broker_2 : mqtt_broker_1;
        mqttClient.setServer(broker, mqtt_port);
        
        Serial.printf("Attempting MQTT connection to %s via WiFi...", broker);
        esp_task_wdt_reset();
        
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
            
            primaryFailCount++;
            if (!useSecondaryBroker && primaryFailCount >= 3) {
                Serial.println("Primary broker failed 3 times. Switching to Secondary Broker!");
                useSecondaryBroker = true;
                primaryFailCount = 0;
            } else if (useSecondaryBroker && primaryFailCount >= 3) {
                Serial.println("Secondary broker failed 3 times. Switching back to Primary Broker!");
                useSecondaryBroker = false;
                primaryFailCount = 0;
            }
        }
    }
}

// --- SIM800L SERIAL & AT COMMAND PARSER (CALLS & DTMF ONLY) ---

void processSimSerial() {
    while (Serial1.available()) {
        char c = (char)Serial1.read();
        if (c == '\r') continue;
        if (c == '\n') {
            if (simLineBuffer.length() > 0) {
                parseSimLine(simLineBuffer);
                simLineBuffer = "";
            }
        } else {
            simLineBuffer += c;
            if (simLineBuffer.length() > 80) simLineBuffer = "";
        }
    }
}

void parseSimLine(const String& line) {
    Serial.printf("[SIM800L] %s\n", line.c_str());

    // Detect incoming call ring
    if (line.indexOf("RING") != -1) {
        simRingDetected = true;
    }
    // Detect Caller ID: +CLIP: "017...",...
    else if (line.indexOf("+CLIP:") != -1) {
        int firstQuote = line.indexOf('"');
        if (firstQuote != -1) {
            int secondQuote = line.indexOf('"', firstQuote + 1);
            if (secondQuote != -1) {
                simCallerNumber = line.substring(firstQuote + 1, secondQuote);
                simCallerNumber.trim();
            }
        }
    }
    // Detect DTMF key: +DTMF: 1
    else if (line.indexOf("+DTMF:") != -1) {
        int idx = line.indexOf("+DTMF:");
        String keyStr = line.substring(idx + 6);
        keyStr.trim();
        if (keyStr.length() > 0) {
            char k = keyStr[0];
            if (k == '\'' || k == '"') {
                if (keyStr.length() > 1) k = keyStr[1];
            }
            if ((k >= '0' && k <= '9') || k == '*' || k == '#') {
                simLastDtmf = k;
                simHasDtmf = true;
            }
        }
    }
    // Detect hangup / disconnect
    else if (line.indexOf("NO CARRIER") != -1 || line.indexOf("BUSY") != -1 || line.indexOf("NO ANSWER") != -1) {
        simNoCarrier = true;
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    pinMode(RELAY_1, OUTPUT); digitalWrite(RELAY_1, RELAY_RELEASE);
    pinMode(RELAY_2, OUTPUT); digitalWrite(RELAY_2, RELAY_RELEASE);
    pinMode(relay_3, OUTPUT); digitalWrite(relay_3, RELAY_RELEASE);
    pinMode(SWITCH_1, INPUT_PULLUP); // Contactor feedback
    pinMode(SWITCH_2, INPUT_PULLUP); // Emergency switch
    pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);

    // Initialize SIM800L on Serial1 (Strictly for Calls & DTMF, NO GPRS)
    pinMode(SIM800_RST, OUTPUT);
    digitalWrite(SIM800_RST, HIGH);
    Serial1.begin(9600, SERIAL_8N1, ESP_RX_FROM_SIM800, ESP_TX_TO_SIM800);
    Serial.println("SIM800L Serial1 Initialized (Calls & DTMF Only, No GPRS)");

    // Pre-configure SIM800L audio, DTMF decoder and caller ID
    delay(500);
    Serial1.println("AT");
    delay(100);
    Serial1.println("ATE0");         // Echo OFF for cleaner serial parsing
    delay(100);
    Serial1.println("AT+CLIP=1");    // Enable Caller ID unsolicited result
    delay(100);
    Serial1.println("AT+DDET=1");    // Enable DTMF decoder
    delay(100);
    Serial1.println("AT+CLVL=100");   // Speaker volume 100%
    delay(100);
    Serial1.println("AT+CMIC=0,15");  // Microphone gain 15 (max)
    delay(100);

    // Initialize DFPlayer on Serial2
    Serial2.begin(9600, SERIAL_8N1, ESP_RX_FROM_DFPLAYER, ESP_TX_TO_DFPLAYER);
    if (!myDFPlayer.begin(Serial2)) {
        Serial.println(F("Unable to begin DFPlayer. Check connection/SD card."));
    } else {
        Serial.println(F("DFPlayer Mini Initialized."));
        myDFPlayer.volume(28); // Volume (0~30)
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
        Serial.println("WiFi Failed to connect. Will retry automatically.");
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

    if (WiFi.status() == WL_CONNECTED) {
        checkForFirmwareUpdate();
    }
}

void loop() {
    esp_task_wdt_reset();
    processSimSerial();
    unsigned long now = millis();
    
    // --- NON-BLOCKING PHONE CALL & DTMF STATE MACHINE ---
    switch (currentCallState) {
        case CALL_IDLE:
            if (simRingDetected) {
                simRingDetected = false;
                activeCaller = simCallerNumber;
                simCallerNumber = "";
                Serial.printf("[CALL] Incoming call detected from: %s\n", activeCaller.length() > 0 ? activeCaller.c_str() : "Unknown");

                // Security: Verify Caller ID against whitelist
                if (enableCallerIdWhitelist && !isAuthorizedCaller(activeCaller)) {
                    Serial.println("[SECURITY] Unauthorized caller rejected! Hanging up...");
                    Serial1.println("ATH");
                    currentCallState = CALL_IDLE;
                } else {
                    Serial.println("[CALL] Answering call...");
                    Serial1.println("ATA");
                    currentCallState = CALL_ANSWERING;
                    callTimer = now;
                }
            }
            break;

        case CALL_ANSWERING:
            // Non-blocking wait for 1 second for audio channel stabilization
            if (now - callTimer >= 1000) {
                Serial.println("[CALL] Call connected. Enabling DTMF and playing welcome audio...");
                Serial1.println("AT+DDET=1");
                Serial1.println("AT+CLVL=100");
                Serial1.println("AT+CMIC=0,15");

                myDFPlayer.volume(28);
                myDFPlayer.play(1); // Track 1 (0001.mp3): Welcome & instruction prompt
                currentCallState = CALL_CONNECTED;
                callTimer = now; // Call duration tracker
            }
            break;

        case CALL_CONNECTED:
            // Check for DTMF keypresses
            if (simHasDtmf) {
                char key = simLastDtmf;
                simHasDtmf = false;
                Serial.printf("[DTMF] Received key: %c\n", key);

                if (key == '1') {
                    Serial.println("[IVR] 1 Pressed: START MOTOR");
                    processCommand("START");
                    myDFPlayer.play(2); // Track 2: Motor started confirmation
                } 
                else if (key == '2') {
                    Serial.println("[IVR] 2 Pressed: STOP MOTOR");
                    processCommand("STOP");
                    myDFPlayer.play(3); // Track 3: Motor stopped confirmation
                } 
                else if (key == '3') {
                    Serial.println("[IVR] 3 Pressed: MOTOR STATUS REPORT");
                    if (getActualMotorRunning()) {
                        Serial.println("[IVR] Motor is ON -> Playing Track 4");
                        myDFPlayer.play(4); // Track 4: Motor is currently ON
                    } else {
                        Serial.println("[IVR] Motor is OFF -> Playing Track 5");
                        myDFPlayer.play(5); // Track 5: Motor is currently OFF
                    }
                } 
                else if (key == '0') {
                    Serial.println("[IVR] 0 Pressed: HANGING UP CALL");
                    currentCallState = CALL_HANGING_UP;
                }
            }

            // Remote party hung up
            if (simNoCarrier) {
                simNoCarrier = false;
                Serial.println("[CALL] Remote party hung up (NO CARRIER)");
                currentCallState = CALL_HANGING_UP;
            }

            // Failsafe auto-hangup after 90 seconds
            if (now - callTimer > 90000) {
                Serial.println("[CALL] 90s timeout reached. Auto-hanging up.");
                currentCallState = CALL_HANGING_UP;
            }
            break;

        case CALL_HANGING_UP:
            myDFPlayer.stop();
            Serial1.println("ATH");
            currentCallState = CALL_IDLE;
            activeCaller = "";
            simRingDetected = false;
            simNoCarrier = false;
            Serial.println("[CALL] Call terminated.");
            break;
    }

    // --- MOTOR FEEDBACK VERIFICATION MONITOR ---
    if (pendingVerificationState != "") {
        if (now - verificationStartTime >= 3000) { // 3 seconds verification window
            bool running = getActualMotorRunning();
            if (pendingVerificationState == "ON") {
                if (running) {
                    lastActionDescription = "Motor Started (Contactor Verified)";
                    Serial.println("[FEEDBACK SUCCESS] Contactor closed! Motor running confirmed.");
                } else {
                    lastActionDescription = "Start Failed (No Contactor Feedback)";
                    Serial.println("[FEEDBACK FAILURE] Warning: Contactor did not close after START pulse!");
                }
            } else if (pendingVerificationState == "OFF") {
                if (!running) {
                    lastActionDescription = "Motor Stopped (Contactor Verified)";
                    Serial.println("[FEEDBACK SUCCESS] Contactor opened! Motor stop confirmed.");
                } else {
                    lastActionDescription = "Stop Failed (Motor Still Running)";
                    Serial.println("[FEEDBACK FAILURE] Warning: Contactor did not release after STOP pulse!");
                }
            }
            pendingVerificationState = "";
            lastStatusUpdate = 0; // Trigger instant dashboard update
        }
    }

    // --- WIFI & MQTT MANAGEMENT (Pure WiFi Only) ---
    if (WiFi.status() == WL_CONNECTED) {
        if (!mqttClient.connected()) {
            reconnectMQTT();
        } else {
            mqttClient.loop();
        }
    } else {
        if (now - lastWifiCheck > wifiCheckInterval) {
            lastWifiCheck = now;
            Serial.println("[WIFI] Reconnecting to WiFi...");
            WiFi.reconnect();
        }
    }

    // --- HANDLE RELAY TIMERS ---
    if (relay1_timer > 0 && now - relay1_timer >= relay_duration) {
        digitalWrite(RELAY_1, RELAY_RELEASE); 
        relay1_timer = 0;
        Serial.println("[ACTION] RELAY_1 (START) Pulse Ended");
    }
    if (relay2_timer > 0 && now - relay2_timer >= relay_duration) {
        digitalWrite(RELAY_2, RELAY_RELEASE); 
        relay2_timer = 0;
        Serial.println("[ACTION] RELAY_2 (STOP) Pulse Ended");
    }
    if (relay3_timer > 0 && now - relay3_timer >= relay_duration) {
        digitalWrite(relay_3, RELAY_RELEASE); 
        relay3_timer = 0;
        Serial.println("[ACTION] RELAY_3 (RESET) Pulse Ended");
    }

    // --- OTA CHECK (WiFi ONLY) ---
    if (now - lastUpdateCheck > updateCheckInterval) {
        lastUpdateCheck = now;
        if (WiFi.status() == WL_CONNECTED) {
            checkForFirmwareUpdate();
        }
    }

    // Check Status and Send Update
    if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
        sendStatus();
    }
}

// --- SECURE OTA FUNCTIONS (WiFi ONLY) ---

bool isValidVersionFormat(const String& ver) {
    int ma = -1, mi = -1, pa = -1;
    if (sscanf(ver.c_str(), "%d.%d.%d", &ma, &mi, &pa) != 3) return false;
    return (ma >= 0 && mi >= 0 && pa >= 0);
}

bool isNewerVersion(String current, String latest) {
    if (!isValidVersionFormat(latest)) return false;
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
    Serial.printf("[OTA] New version %s available (Current: %s). Initiating update via WiFi...\n", 
                  latestVersion.c_str(), currentFirmwareVersion);
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
    if (isValidVersionFormat(latestVersion)) {
      return latestVersion;
    }
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
    if (contentLength > 100000 && contentLength < 2000000) {
      Serial.printf("[OTA] Valid firmware size confirmed: %d bytes. Writing to flash...\n", contentLength);
      WiFiClient* stream = http.getStreamPtr();
      if (startOTAUpdate(stream, contentLength)) {
        Serial.println("[OTA SUCCESS] Firmware updated! Restarting ESP32...");
        delay(1000);
        ESP.restart();
      }
    } else {
      Serial.printf("[OTA REJECT] Invalid firmware binary size (%d bytes).\n", contentLength);
    }
  }
  http.end();
}

bool startOTAUpdate(WiFiClient* client, int contentLength) {
  if (!Update.begin(contentLength)) {
    Serial.printf("[OTA ERROR] Not enough flash space for update: %s\n", Update.errorString());
    return false;
  }

  size_t written = 0;
  const unsigned long timeoutDuration = 120 * 1000;
  unsigned long lastDataTime = millis();

  while (written < (size_t)contentLength) {
    esp_task_wdt_reset();
    if (client->available()) {
      uint8_t buffer[256];
      size_t len = client->read(buffer, sizeof(buffer));
      if (len > 0) {
        size_t writtenChunk = Update.write(buffer, len);
        if (writtenChunk != len) {
          Serial.printf("[OTA FLASH ERROR] Flash write failed! Expected %d, wrote %d\n", len, writtenChunk);
          Update.abort();
          return false;
        }
        written += len;
        lastDataTime = millis();
      }
    }
    if (millis() - lastDataTime > timeoutDuration) {
      Serial.println("[OTA ERROR] Stream timeout during download.");
      Update.abort();
      return false;
    }
    yield();
  }

  if (written != (size_t)contentLength) {
    Serial.println("[OTA ERROR] Downloaded size mismatch.");
    Update.abort();
    return false;
  }

  if (!Update.end(true)) {
    Serial.printf("[OTA ERROR] Update.end() failed: %s\n", Update.errorString());
    return false;
  }

  return true;
}