// =================================================================
// Mutho-Sech Smart Pump Controller v8.0 (Production 24/7 Firmware)
// Hardware: ESP32 + SIM800L (Hardware Serial1) + DFPlayer Mini (Hardware Serial2)
// Pure WiFi Internet for MQTT/OTA + Dedicated SIM800L for Calls, DTMF & SMS
// Dynamic Mobile Number Registration & Deletion from Dashboard with NVS Persistence
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
#include <Preferences.h>
#include <Ticker.h>
#include <DFRobotDFPlayerMini.h>
#include <vector>

// --- Configuration: Primary & Secondary MQTT Brokers (WiFi Only) ---
const char* mqtt_broker_1 = "mosquitto-muthosech.espserver.site"; // Primary Broker
const char* mqtt_broker_2 = "broker.emqx.io";                     // Secondary Broker
const uint16_t mqtt_port = 1883; 
const char* mqtt_user = ""; 
const char* mqtt_pass = ""; 

#define WDT_TIMEOUT 60 // 60 Seconds Watchdog

// Firmware Update URLs (via WiFi)
const char* firmwareUrl = "https://github.com/shohidmax/pumpv3/releases/download/shohidpump/abbu_pump_online.ino.bin";
const char* versionUrl = "https://raw.githubusercontent.com/shohidmax/pumpv3/refs/heads/main/version.txt";
const char* currentFirmwareVersion = "2.5.0"; 

// --- PIN DEFINITIONS (User Hardware) ---
#define RELAY_1 25   // START Pulse Output (Contactor Coil Start)
#define RELAY_2 26   // STOP Pulse Output (Contactor Coil Stop)
#define relay_3 32   // RESET Pulse Output
#define SWITCH_1 23  // Motor Running Feedback Input (Contactor Aux NO -> GND)
#define SWITCH_2 22  // Emergency Stop Switch Input (LOW = Normal, HIGH = Emergency)
#define LED_PIN 2    // Built-in Status LED

// --- SIM800L & DFPLAYER PINS (ESP32 Hardware UARTs) ---
#define ESP_RX_FROM_SIM800 16 // Connects to SIM800L TX
#define ESP_TX_TO_SIM800 17   // Connects to SIM800L RX
#define SIM800_RST 33         // SIM800L Reset Pin
#define ESP_RX_FROM_DFPLAYER 14 // Connects to DFPlayer TX
#define ESP_TX_TO_DFPLAYER 27   // Connects to DFPlayer RX

// --- RELAY POLARITY CONFIGURATION (Active-HIGH) ---
#define RELAY_TRIGGER HIGH
#define RELAY_RELEASE LOW

// --- MOTOR FEEDBACK CONFIGURATION ---
#define USE_PHYSICAL_FEEDBACK true
#define FEEDBACK_ACTIVE_LEVEL LOW // Contactor pulled in = LOW

// --- AUTHORIZED PHONE NUMBERS (Stored in ESP32 NVS) ---
Preferences prefs;
std::vector<String> allowedNumbers;

const char* DEFAULT_NUMBERS[] = {
  "+8801793496030",
  "+8801718677688",
  "+8801728754140",
  "+8801797723909",
  "+8801723349796"
};
const uint8_t DEFAULT_NUM_COUNT = sizeof(DEFAULT_NUMBERS) / sizeof(DEFAULT_NUMBERS[0]);

// --- SIM800L RUNTIME STATE ---
char rxBuffer[256];
uint8_t rxIndex = 0;
char activeNumber[24] = "";
bool isCallActive = false;
bool isSmsContentNext = false;
unsigned long lastSimCheckTime = 0;
const unsigned long SIM_CHECK_INTERVAL = 300000UL; // 5 minutes periodic maintenance

// --- GLOBAL CLIENTS & BUFFERS ---
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
const int relay_duration = 1000; // 1 second pulse

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

unsigned long lastUpdateCheck = 0;
const unsigned long updateCheckInterval = 5 * 60 * 1000;
unsigned long lastWifiCheck = 0;
const unsigned long wifiCheckInterval = 10000;

Ticker blinker;

// --- FORWARD DECLARATIONS ---
void safeDelay(unsigned long ms);
void initSIM800L();
void processSimSerial();
void processLine(char* line);
bool verifyAndSetNumber(const char* text);
void handleSMSCommand(const char* sms);
void handleDTMF(char digit);
void sendSMS(const char* number, const char* msg);
void playTrack(uint8_t trackNum, unsigned long waitMs = 3000);
void loadNumbersFromNVS();
void saveNumbersToNVS();
bool addAuthorizedNumber(const String& num);
bool removeAuthorizedNumber(const String& num);
bool getActualMotorRunning();
void processCommand(String command);
void sendStatus();
void reconnectMQTT();
void checkForFirmwareUpdate();
String fetchLatestVersion();
void downloadAndApplyFirmware();
bool startOTAUpdate(WiFiClient* client, int contentLength);

// --- NVS PHONE NUMBER WHITELIST MANAGEMENT ---
void loadNumbersFromNVS() {
    prefs.begin("pump_auth", false);
    int count = prefs.getInt("count", -1);
    allowedNumbers.clear();

    if (count <= 0) {
        // Initialize default numbers if first boot
        for (uint8_t i = 0; i < DEFAULT_NUM_COUNT; i++) {
            allowedNumbers.push_back(String(DEFAULT_NUMBERS[i]));
        }
        saveNumbersToNVS();
        Serial.println(F("[NVS] Initialized default authorized numbers list."));
    } else {
        for (int i = 0; i < count; i++) {
            String key = "num" + String(i);
            String num = prefs.getString(key.c_str(), "");
            if (num.length() > 0) {
                allowedNumbers.push_back(num);
            }
        }
        Serial.printf("[NVS] Loaded %d authorized numbers from storage.\n", allowedNumbers.size());
    }
    prefs.end();
}

void saveNumbersToNVS() {
    prefs.begin("pump_auth", false);
    prefs.clear();
    prefs.putInt("count", allowedNumbers.size());
    for (size_t i = 0; i < allowedNumbers.size(); i++) {
        String key = "num" + String(i);
        prefs.putString(key.c_str(), allowedNumbers[i]);
    }
    prefs.end();
    Serial.printf("[NVS] Saved %d authorized numbers to NVS.\n", allowedNumbers.size());
}

bool addAuthorizedNumber(const String& num) {
    String clean = num;
    clean.trim();
    if (clean.length() < 8) return false;

    for (const auto& existing : allowedNumbers) {
        if (existing == clean) return true; // Already exists
    }
    allowedNumbers.push_back(clean);
    saveNumbersToNVS();
    return true;
}

bool removeAuthorizedNumber(const String& num) {
    String clean = num;
    clean.trim();
    bool removed = false;
    for (auto it = allowedNumbers.begin(); it != allowedNumbers.end(); ) {
        if (*it == clean || clean.indexOf(*it) != -1 || it->indexOf(clean) != -1) {
            it = allowedNumbers.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed) {
        saveNumbersToNVS();
    }
    return removed;
}

// Check if incoming phone matches any registered authorized number
bool verifyAndSetNumber(const char* text) {
    for (const auto& num : allowedNumbers) {
        if (strstr(text, num.c_str()) != NULL) {
            strncpy(activeNumber, num.c_str(), sizeof(activeNumber) - 1);
            activeNumber[sizeof(activeNumber) - 1] = '\0';
            return true;
        }
    }
    return false;
}

// --- Watchdog-Safe Delay Function ---
void safeDelay(unsigned long ms) {
    unsigned long start = millis();
    while (millis() - start < ms) {
        esp_task_wdt_reset();
        delay(10);
    }
}

// --- DFPLAYER AUDIO PLAYBACK ---
void playTrack(uint8_t trackNum, unsigned long waitMs) {
    Serial.printf("[DFPLAYER] Playing Track %d\n", trackNum);
    myDFPlayer.play(trackNum);
    if (waitMs > 0) {
        safeDelay(waitMs);
    }
}

// --- SIM800L INITIALIZATION ---
void initSIM800L() {
    Serial.println(F("[SIM800L] Initializing modem..."));
    pinMode(SIM800_RST, OUTPUT);
    digitalWrite(SIM800_RST, HIGH);
    safeDelay(200);

    Serial1.println(F("AT"));
    safeDelay(500);
    Serial1.println(F("ATE0"));               // Turn off Echo
    safeDelay(500);
    Serial1.println(F("AT+CLIP=1"));          // Enable Caller ID
    safeDelay(500);
    Serial1.println(F("AT+DDET=1"));          // Enable DTMF decoder
    safeDelay(500);
    Serial1.println(F("AT+CLVL=100"));         // Max Volume
    safeDelay(500);
    Serial1.println(F("AT+CMIC=0,15"));        // Microphone gain
    safeDelay(500);
    Serial1.println(F("AT+CMGF=1"));           // SMS text mode
    safeDelay(500);
    Serial1.println(F("AT+CNMI=2,2,0,0,0"));   // Direct SMS routing to serial
    safeDelay(500);
    Serial1.println(F("AT+CMGD=1,4"));         // Clean old SMS inbox
    safeDelay(500);
    Serial.println(F("[SIM800L] Ready for Voice & DTMF & SMS!"));
}

// --- SIM800L NON-BLOCKING SERIAL STREAM READER ---
void processSimSerial() {
    while (Serial1.available() > 0) {
        char c = (char)Serial1.read();
        if (c == '\n') {
            rxBuffer[rxIndex] = '\0';
            if (rxIndex > 0) {
                processLine(rxBuffer);
            }
            rxIndex = 0;
        } else if (c != '\r') {
            if (rxIndex < sizeof(rxBuffer) - 1) {
                rxBuffer[rxIndex++] = c;
            } else {
                rxBuffer[rxIndex] = '\0';
                processLine(rxBuffer);
                rxIndex = 0;
            }
        }
    }
}

// --- SIM800L LINE-BY-LINE PROTOCOL HANDLER ---
void processLine(char* line) {
    Serial.printf("[SIM800L] %s\n", line);

    // 1. If previous line was +CMT:, this line is the SMS message body
    if (isSmsContentNext) {
        isSmsContentNext = false;
        handleSMSCommand(line);
        return;
    }

    // 2. Incoming call with Caller ID (+CLIP: "+88017...",145,...)
    if (strstr(line, "+CLIP:") != NULL) {
        if (verifyAndSetNumber(line)) {
            if (!isCallActive) {
                Serial.printf("[CALL] Authorized call from %s. Answering...\n", activeNumber);
                Serial1.println(F("ATA")); // Answer call
                isCallActive = true;
                safeDelay(600);
                // Welcome tunes (Track 3 followed by Track 4)
                playTrack(3, 3000);
                playTrack(4, 3000);
            }
        } else {
            Serial.println(F("[CALL] Unauthorized number! Hanging up immediately (ATH)..."));
            Serial1.println(F("ATH")); // Reject unauthorized call
            isCallActive = false;
        }
        return;
    }

    // 3. Call disconnected (NO CARRIER / BUSY / NO ANSWER)
    if (strstr(line, "NO CARRIER") != NULL || strstr(line, "BUSY") != NULL || strstr(line, "NO ANSWER") != NULL) {
        Serial.println(F("[CALL] Remote party disconnected or call ended."));
        isCallActive = false;
        myDFPlayer.stop();
        return;
    }

    // 4. In-call DTMF Key Detection (+DTMF: 1)
    if (isCallActive && strstr(line, "+DTMF:") != NULL) {
        char* colonPtr = strchr(line, ':');
        if (colonPtr != NULL) {
            colonPtr++;
            while (*colonPtr == ' ' || *colonPtr == '"' || *colonPtr == '\'') colonPtr++;
            handleDTMF(*colonPtr);
        }
        return;
    }

    // 5. Incoming SMS header (+CMT: "+88017...", "", "...")
    if (strstr(line, "+CMT:") != NULL) {
        if (verifyAndSetNumber(line)) {
            Serial.printf("[SMS] Authorized SMS incoming from: %s\n", activeNumber);
            isSmsContentNext = true;
        } else {
            Serial.println(F("[SMS] Ignored SMS from unauthorized sender."));
            isSmsContentNext = false;
        }
        return;
    }
}

// --- DTMF KEY ACTION HANDLER ---
void handleDTMF(char digit) {
    Serial.printf("[DTMF] Button Pressed: %c\n", digit);

    if (digit == '1') {
        if (!getActualMotorRunning()) {
            Serial.println(F("[DTMF] Key 1: START MOTOR"));
            processCommand("START");
            playTrack(7, 3000); // Track 7: Motor Started confirmation
            sendSMS(activeNumber, "MOTOR ON (VIA CALL)");
        } else {
            Serial.println(F("[DTMF] Motor already ON"));
            playTrack(7, 3000);
        }
    } 
    else if (digit == '2') {
        if (getActualMotorRunning()) {
            Serial.println(F("[DTMF] Key 2: STOP MOTOR"));
            processCommand("STOP");
            playTrack(10, 3000); // Track 10: Motor Stopped confirmation
            sendSMS(activeNumber, "MOTOR OFF (VIA CALL)");
        } else {
            Serial.println(F("[DTMF] Motor already OFF"));
            playTrack(10, 3000);
        }
    } 
    else if (digit == '3') {
        Serial.println(F("[DTMF] Key 3: STATUS REPORT"));
        if (getActualMotorRunning()) {
            playTrack(14, 3000); // Track 14: Motor is Running
        } else {
            playTrack(11, 3000); // Track 11: Motor is Stopped
        }
    } 
    else if (digit == '0') {
        Serial.println(F("[DTMF] Key 0: HANGUP"));
        Serial1.println(F("ATH"));
        isCallActive = false;
        myDFPlayer.stop();
    }
}

// --- SMS COMMAND PROCESSOR ---
void handleSMSCommand(const char* sms) {
    Serial.printf("[SMS COMMAND] Received: '%s' from %s\n", sms, activeNumber);

    if (strstr(sms, "MOTOR_ON") != NULL) {
        if (!getActualMotorRunning()) {
            processCommand("START");
        }
        playTrack(7, 3000);
        sendSMS(activeNumber, "MOTOR IS NOW ON");
    } 
    else if (strstr(sms, "MOTOR_OFF") != NULL) {
        if (getActualMotorRunning()) {
            processCommand("STOP");
        }
        playTrack(10, 3000);
        sendSMS(activeNumber, "MOTOR IS NOW OFF");
    } 
    else if (strstr(sms, "STTS") != NULL) {
        if (getActualMotorRunning()) {
            sendSMS(activeNumber, "STATUS: MOTOR IS ON");
        } else {
            sendSMS(activeNumber, "STATUS: MOTOR IS OFF");
        }
    } 
    else if (strstr(sms, "ETC") != NULL) {
        sendSMS(activeNumber, "Electricity Available");
    } 
    else if (strstr(sms, "list") != NULL) {
        String listMsg = "ALLOWED NUMBERS:\n";
        for (const auto& num : allowedNumbers) {
            listMsg += num + "\n";
        }
        sendSMS(activeNumber, listMsg.c_str());
    }
}

// --- SEND SMS HELPER ---
void sendSMS(const char* number, const char* msg) {
    if (strlen(number) < 5) return;
    Serial.printf("[SMS] Sending SMS to %s: %s\n", number, msg);

    Serial1.println(F("AT+CMGF=1"));
    safeDelay(300);

    Serial1.print(F("AT+CMGS=\""));
    Serial1.print(number);
    Serial1.println(F("\""));
    safeDelay(300);

    Serial1.print(msg);
    safeDelay(200);
    Serial1.write(0x1A); // Ctrl+Z EOF
    safeDelay(1000);
}

// --- PHYSICAL MOTOR FEEDBACK HELPER ---
bool getActualMotorRunning() {
    if (USE_PHYSICAL_FEEDBACK) {
        return (digitalRead(SWITCH_1) == FEEDBACK_ACTIVE_LEVEL);
    } else {
        return (stableMotorState == "ON");
    }
}

// --- HARDWARE COMMAND PROCESSOR ---
void processCommand(String command) {
    unsigned long now = millis();
    bool isEmergencyActive = (digitalRead(SWITCH_2) == HIGH);

    if (command == "RELAY_1" || command == "ON" || command == "START") {
        if (isEmergencyActive) {
            Serial.println(F("[SAFETY] START Rejected: Emergency Stop switch is ACTIVE!"));
            lastActionDescription = "START Rejected (Emergency Active)";
            lastStatusUpdate = 0;
            return;
        }
        if (getActualMotorRunning()) {
            Serial.println(F("[SAFETY] Motor is ALREADY RUNNING. START pulse ignored."));
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
        Serial.println(F("[ACTION] RELAY_1 (START) Pulse Active -> HIGH for 1 sec"));
    } 
    else if (command == "RELAY_2" || command == "OFF" || command == "STOP") {
        if (!getActualMotorRunning() && stableMotorState == "OFF") {
            Serial.println(F("[SAFETY] Motor is ALREADY STOPPED. STOP pulse ignored."));
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
        Serial.println(F("[ACTION] RELAY_2 (STOP) Pulse Active -> HIGH for 1 sec"));
    } 
    else if (command == "RESET") {
        digitalWrite(relay_3, RELAY_TRIGGER);
        relay3_timer = now;
        lastActionDescription = "System Reset Pulse";
        Serial.println(F("[ACTION] RELAY_3 (RESET) Active -> HIGH for 1 sec"));
    } 
    else if (command == "LED_ON") {
        digitalWrite(LED_PIN, HIGH);
        Serial.println(F("[ACTION] Built-in LED turned ON remotely"));
    } 
    else if (command == "LED_OFF") {
        digitalWrite(LED_PIN, LOW);
        Serial.println(F("[ACTION] Built-in LED turned OFF remotely"));
    } 
    else if (command == "RESTART_ESP") {
        Serial.println(F("[ACTION] Restarting ESP32..."));
        delay(500);
        ESP.restart();
    } 
    else if (command == "CHECK_UPDATE") {
        Serial.println(F("[ACTION] Checking for firmware updates via WiFi..."));
        checkForFirmwareUpdate();
    }
    lastStatusUpdate = 0; 
}

// --- SEND REALTIME STATUS OVER MQTT ---
void sendStatus() {
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
        payload["gsmMode"] = "Voice Calls & DTMF (No GPRS)";

        // Include current authorized numbers list
        JsonArray numbersArr = payload.createNestedArray("authorizedNumbers");
        for (const auto& num : allowedNumbers) {
            numbersArr.add(num);
        }
        
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

// --- MQTT INCOMING MESSAGE CALLBACK ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    esp_task_wdt_reset();

    if (length > 512) {
        Serial.println(F("[MQTT SECURITY] Payload exceeds max allowed size (512 bytes). Dropping."));
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

            // 1. Motor & Hardware Control Commands
            if (command == "ON" || command == "OFF" || command == "START" || command == "STOP" || 
                command == "RESET" || command == "LED_ON" || command == "LED_OFF" || 
                command == "RESTART_ESP" || command == "CHECK_UPDATE") {
                Serial.printf("[COMMAND] Authorized command processing: %s\n", command.c_str());
                processCommand(command);
            } 
            // 2. Add Mobile Number to Whitelist from Dashboard
            else if (command == "ADD_NUMBER") {
                if (doc["number"].is<String>()) {
                    String newNum = doc["number"].as<String>();
                    addAuthorizedNumber(newNum);
                    Serial.printf("[AUTH] Added phone number: %s\n", newNum.c_str());
                    lastStatusUpdate = 0;
                    sendStatus();
                }
            }
            // 3. Delete Mobile Number from Whitelist from Dashboard
            else if (command == "DELETE_NUMBER") {
                if (doc["number"].is<String>()) {
                    String delNum = doc["number"].as<String>();
                    removeAuthorizedNumber(delNum);
                    Serial.printf("[AUTH] Deleted phone number: %s\n", delNum.c_str());
                    lastStatusUpdate = 0;
                    sendStatus();
                }
            }
            // 4. Batch Sync Phone Numbers from Dashboard
            else if (command == "SET_NUMBERS") {
                if (doc["authorizedNumbers"].is<JsonArray>()) {
                    allowedNumbers.clear();
                    for (JsonVariant v : doc["authorizedNumbers"].as<JsonArray>()) {
                        allowedNumbers.push_back(v.as<String>());
                    }
                    saveNumbersToNVS();
                    Serial.println(F("[AUTH] Synced full numbers list from dashboard"));
                    lastStatusUpdate = 0;
                    sendStatus();
                }
            } else {
                Serial.printf("[SECURITY] Rejected unrecognized command: %s\n", command.c_str());
            }
        } else {
            Serial.println(F("[SECURITY] Malformed JSON payload received on command topic."));
        }
    }
}

// --- MQTT RECONNECT LOGIC WITH HA BROKER FAILOVER ---
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

void tick() {
    int state = digitalRead(LED_PIN);
    digitalWrite(LED_PIN, !state);
}

void configModeCallback(WiFiManager *myWiFiManager) {
    Serial.println("Entered WiFi config mode");
    Serial.println(WiFi.softAPIP());
    blinker.attach(0.3, tick);
}

// --- SETUP ---
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println(F("\n=============================================="));
    Serial.println(F("  Mutho-Sech Smart Pump Controller Starting   "));
    Serial.println(F("=============================================="));

    // 1. Setup Relay & Switch Pins
    pinMode(RELAY_1, OUTPUT); digitalWrite(RELAY_1, RELAY_RELEASE);
    pinMode(RELAY_2, OUTPUT); digitalWrite(RELAY_2, RELAY_RELEASE);
    pinMode(relay_3, OUTPUT); digitalWrite(relay_3, RELAY_RELEASE);
    pinMode(SWITCH_1, INPUT_PULLUP); // Contactor feedback
    pinMode(SWITCH_2, INPUT_PULLUP); // Emergency stop switch
    pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);

    // 2. Initialize NVS and load authorized numbers
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    loadNumbersFromNVS();

    // 3. Initialize SIM800L on Hardware Serial1 (Pins 16 & 17)
    Serial1.begin(9600, SERIAL_8N1, ESP_RX_FROM_SIM800, ESP_TX_TO_SIM800);
    initSIM800L();

    // 4. Initialize DFPlayer Mini on Hardware Serial2 (Pins 14 & 27)
    Serial2.begin(9600, SERIAL_8N1, ESP_RX_FROM_DFPLAYER, ESP_TX_TO_DFPLAYER);
    if (!myDFPlayer.begin(Serial2)) {
        Serial.println(F("[DFPLAYER] Unable to begin DFPlayer. Check SD card."));
    } else {
        Serial.println(F("[DFPLAYER] DFPlayer Mini Initialized!"));
        myDFPlayer.volume(28); // Volume 0-30
    }

    // Play startup tune (Track 1)
    playTrack(1, 3000);

    // 5. Watchdog Timer Configuration
    esp_task_wdt_deinit();
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_config);
    esp_task_wdt_add(NULL);

    // 6. WiFi Setup via WiFiManager
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    
    WiFiManager wm;
    wm.setAPCallback(configModeCallback);
    wm.setConfigPortalTimeout(60); 

    Serial.println(F("Attempting WiFi Connection..."));
    if (!wm.autoConnect("Mutho-Sech")) {
        Serial.println(F("WiFi Failed to connect. Will retry automatically."));
    } else {
        Serial.println(F("WiFi Connected Successfully!"));
    }

    blinker.detach();
    digitalWrite(LED_PIN, HIGH);

    macAddress = WiFi.macAddress();
    if (macAddress == "" || macAddress == "00:00:00:00:00:00") {
        uint64_t chipid = ESP.getEfuseMac(); 
        char macStr[13];
        snprintf(macStr, 13, "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
        macAddress = String(macStr);
    } else {
        macAddress.replace(":", "");
    }

    topicStatus = "device/" + macAddress + "/status";
    topicCommand = "device/" + macAddress + "/command";
    Serial.println("Device MAC: " + macAddress);

    mqttClient.setBufferSize(512);
    mqttClient.setKeepAlive(10);
    mqttClient.setSocketTimeout(15);
    mqttClient.setCallback(mqttCallback);

    if (WiFi.status() == WL_CONNECTED) {
        checkForFirmwareUpdate();
    }

    Serial.println(F("System Ready for 24/7 Operation."));
}

// --- MAIN LOOP ---
void loop() {
    esp_task_wdt_reset();
    unsigned long now = millis();

    // 1. Process SIM800L serial data (Calls, DTMF, SMS)
    processSimSerial();

    // 2. Periodic SIM800L health check & inbox cleanup (Every 5 minutes)
    if (now - lastSimCheckTime >= SIM_CHECK_INTERVAL) {
        lastSimCheckTime = now;
        if (!isCallActive) {
            Serial.println(F("[SIM800L] Periodic check & cleaning SMS inbox..."));
            Serial1.println(F("AT"));
            safeDelay(200);
            Serial1.println(F("AT+DDET=1"));
            safeDelay(200);
            Serial1.println(F("AT+CMGF=1"));
            safeDelay(200);
            Serial1.println(F("AT+CNMI=2,2,0,0,0"));
            safeDelay(200);
            Serial1.println(F("AT+CMGD=1,4")); // Delete all read/sent/received SMS to prevent overflow
        }
    }

    // 3. Contactor Feedback Verification Window (3 seconds)
    if (pendingVerificationState != "") {
        if (now - verificationStartTime >= 3000) {
            bool running = getActualMotorRunning();
            if (pendingVerificationState == "ON") {
                if (running) {
                    lastActionDescription = "Motor Started (Contactor Verified)";
                    Serial.println(F("[FEEDBACK] Contactor closed! Motor running confirmed."));
                } else {
                    lastActionDescription = "Start Failed (No Contactor Feedback)";
                    Serial.println(F("[FEEDBACK] Warning: Contactor did not close after START pulse!"));
                }
            } else if (pendingVerificationState == "OFF") {
                if (!running) {
                    lastActionDescription = "Motor Stopped (Contactor Verified)";
                    Serial.println(F("[FEEDBACK] Contactor opened! Motor stop confirmed."));
                } else {
                    lastActionDescription = "Stop Failed (Motor Still Running)";
                    Serial.println(F("[FEEDBACK] Warning: Contactor did not release after STOP pulse!"));
                }
            }
            pendingVerificationState = "";
            lastStatusUpdate = 0; // Trigger immediate dashboard update
        }
    }

    // 4. WiFi & MQTT Connection Handling
    if (WiFi.status() == WL_CONNECTED) {
        if (!mqttClient.connected()) {
            reconnectMQTT();
        } else {
            mqttClient.loop();
        }
    } else {
        if (now - lastWifiCheck > wifiCheckInterval) {
            lastWifiCheck = now;
            Serial.println(F("[WIFI] Reconnecting to WiFi..."));
            WiFi.reconnect();
        }
    }

    // 5. Handle Relay Pulse Timers (1 second pulse)
    if (relay1_timer > 0 && now - relay1_timer >= relay_duration) {
        digitalWrite(RELAY_1, RELAY_RELEASE); 
        relay1_timer = 0;
        Serial.println(F("[PULSE] RELAY_1 (START) Pulse Finished"));
    }
    if (relay2_timer > 0 && now - relay2_timer >= relay_duration) {
        digitalWrite(RELAY_2, RELAY_RELEASE); 
        relay2_timer = 0;
        Serial.println(F("[PULSE] RELAY_2 (STOP) Pulse Finished"));
    }
    if (relay3_timer > 0 && now - relay3_timer >= relay_duration) {
        digitalWrite(relay_3, RELAY_RELEASE); 
        relay3_timer = 0;
        Serial.println(F("[PULSE] RELAY_3 (RESET) Pulse Finished"));
    }

    // 6. Check for OTA Updates via WiFi
    if (now - lastUpdateCheck > updateCheckInterval) {
        lastUpdateCheck = now;
        if (WiFi.status() == WL_CONNECTED) {
            checkForFirmwareUpdate();
        }
    }

    // 7. Push telemetry status update
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
        Serial.printf("[OTA] New version %s available (Current: %s). Updating...\n", 
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
            Serial.printf("[OTA] Firmware size confirmed: %d bytes. Writing to flash...\n", contentLength);
            WiFiClient* stream = http.getStreamPtr();
            if (startOTAUpdate(stream, contentLength)) {
                Serial.println(F("[OTA SUCCESS] Firmware updated! Restarting ESP32..."));
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
        Serial.printf("[OTA ERROR] Not enough flash space: %s\n", Update.errorString());
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
                    Serial.printf("[OTA FLASH ERROR] Expected %d, wrote %d\n", len, writtenChunk);
                    Update.abort();
                    return false;
                }
                written += len;
                lastDataTime = millis();
            }
        }
        if (millis() - lastDataTime > timeoutDuration) {
            Serial.println(F("[OTA ERROR] Download timeout"));
            Update.abort();
            return false;
        }
        yield();
    }

    if (written != (size_t)contentLength) {
        Serial.println(F("[OTA ERROR] Downloaded size mismatch"));
        Update.abort();
        return false;
    }

    if (!Update.end(true)) {
        Serial.printf("[OTA ERROR] Update.end() failed: %s\n", Update.errorString());
        return false;
    }

    return true;
}