// ============================================
// HASKE ATS - ESP8266 CONTROLLER (web app version, no Blynk)
// ============================================
// Converted from the original Blynk sketch. Same pins, same relay logic,
// same saved counters (existing hours in EEPROM are kept).
//
// The board sends its readings when something changes (mains or generator
// appears/disappears, a relay switches) and otherwise sends one small
// heartbeat a minute. In between it only asks "any commands for me?" every
// 5 seconds, so switch commands stay quick without re-sending every reading
// all the time. Each command is executed once and confirmed back, so the
// dashboard only shows
// "Confirmed by device" after the board has really switched.
//
// Libraries (Arduino Library Manager):
//   - WiFiManager (tzapu)      - same as before
//   - ArduinoJson (v7, Benoit Blanchon)
// Remove the Blynk library include; it is no longer used.
// ============================================

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>

// ============================================
// WEB APP CONNECTION  (from Device Management)
// ============================================

const char* SERVER_URL = "https://project--98cb423f-2051-44fa-847e-467f7c328e84.lovable.app";
const char* DEVICE_ID  = "HASKE-001";
const char* DEVICE_KEY = "412931bc2c2085b9a08ae75612d4a458664bc6f4cf8ca102";
const char* FIRMWARE   = "haske-esp8266-2.1";

// ============================================
// GPIO CONFIGURATION (unchanged)
// ============================================

#define PHCN_SENSOR_PIN 4
#define GEN_SENSOR_PIN  5

#define RELAY1_PIN 12   // AUTO
#define RELAY2_PIN 13   // OFF PHCN
#define RELAY3_PIN 14   // OFF LOAD

#define RELAY_ON HIGH
#define RELAY_OFF LOW

// ============================================
// STORAGE AND RUNTIME SETTINGS (unchanged)
// ============================================

#define EEPROM_SIZE 256
#define RUNTIME_MAGIC 0x4841534B
#define DEFAULT_SERVICE_HOURS 200UL
#define MAX_SERVICE_HOURS 5000UL
#define SAVE_INTERVAL 300000UL     // save counters every 5 minutes
#define TIMEZONE_OFFSET 3600       // Nigeria: UTC+1

#define REPORT_INTERVAL 60000UL      // full readings heartbeat every 60 s
#define COMMAND_POLL_INTERVAL 5000UL // ask for waiting commands every 5 s
#define HISTORY_INTERVAL 30000UL     // upload one queued daily record every 30 s

// ============================================
// STATE
// ============================================

bool phcnState = false;
bool genState = false;

bool relay1State = false;
bool relay2State = false;
bool relay3State = false;

int lastPHCNState = -1;
int lastGENState = -1;

uint32_t phcnTotalSeconds = 0;
uint32_t genTotalSeconds = 0;
uint32_t genServiceSeconds = 0;
uint32_t phcnRemainderMs = 0;
uint32_t genRemainderMs = 0;

uint32_t serviceLimitSeconds = DEFAULT_SERVICE_HOURS * 3600UL;

uint32_t lastRuntimeUpdate = 0;
uint32_t lastRuntimeSave = 0;
uint32_t lastReport = 0;
uint32_t lastCommandPoll = 0;
uint32_t lastHistory = 0;

uint32_t dailyPHCNSeconds = 0;
uint32_t dailyGENSeconds = 0;
uint32_t dailyDateKey = 0;

bool ntpTimeAvailable = false;
bool littleFSAvailable = false;

// The web app now sends the service-due alert (in-app + email to owner and technician).
// This flag is kept only so the saved EEPROM layout stays the same.
bool serviceNotificationSent = false;

// Recently executed command IDs, so a repeated command never switches twice.
String doneIds[8];
String doneStatus[8];
int doneNext = 0;

// ============================================
// EEPROM DATA STRUCTURE (unchanged layout)
// ============================================

struct RuntimeData {
  uint32_t magic;
  uint32_t phcnSeconds;
  uint32_t genTotalSeconds;
  uint32_t genServiceSeconds;
  uint8_t notificationSent;
  uint32_t serviceLimitSeconds;
  uint32_t dailyDateKey;
  uint32_t dailyPHCNSeconds;
  uint32_t dailyGENSeconds;
};

void saveRuntime() {
  RuntimeData data;
  data.magic = RUNTIME_MAGIC;
  data.phcnSeconds = phcnTotalSeconds;
  data.genTotalSeconds = genTotalSeconds;
  data.genServiceSeconds = genServiceSeconds;
  data.notificationSent = serviceNotificationSent ? 1 : 0;
  data.serviceLimitSeconds = serviceLimitSeconds;
  data.dailyDateKey = dailyDateKey;
  data.dailyPHCNSeconds = dailyPHCNSeconds;
  data.dailyGENSeconds = dailyGENSeconds;
  EEPROM.put(0, data);
  EEPROM.commit();
  Serial.println("Runtime data saved.");
}

void loadRuntime() {
  EEPROM.begin(EEPROM_SIZE);
  RuntimeData data;
  EEPROM.get(0, data);

  if (data.magic == RUNTIME_MAGIC) {
    phcnTotalSeconds = data.phcnSeconds;
    genTotalSeconds = data.genTotalSeconds;
    genServiceSeconds = data.genServiceSeconds;
    serviceNotificationSent = (data.notificationSent == 1);
    dailyDateKey = data.dailyDateKey;
    dailyPHCNSeconds = data.dailyPHCNSeconds;
    dailyGENSeconds = data.dailyGENSeconds;
    if (data.serviceLimitSeconds >= 3600UL && data.serviceLimitSeconds <= MAX_SERVICE_HOURS * 3600UL) {
      serviceLimitSeconds = data.serviceLimitSeconds;
    } else {
      serviceLimitSeconds = DEFAULT_SERVICE_HOURS * 3600UL;
    }
    Serial.println("Previous runtime restored.");
  } else {
    phcnTotalSeconds = 0;
    genTotalSeconds = 0;
    genServiceSeconds = 0;
    dailyDateKey = 0;
    dailyPHCNSeconds = 0;
    dailyGENSeconds = 0;
    serviceLimitSeconds = DEFAULT_SERVICE_HOURS * 3600UL;
    serviceNotificationSent = false;
    Serial.println("New runtime storage initialized.");
    saveRuntime();
  }
}

// ============================================
// GPIO
// ============================================

void setupPins() {
  digitalWrite(RELAY1_PIN, RELAY_OFF);
  digitalWrite(RELAY2_PIN, RELAY_OFF);
  digitalWrite(RELAY3_PIN, RELAY_OFF);
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(RELAY3_PIN, OUTPUT);
  pinMode(PHCN_SENSOR_PIN, INPUT);
  pinMode(GEN_SENSOR_PIN, INPUT);
  relay1State = false;
  relay2State = false;
  relay3State = false;
  Serial.println("GPIO initialized.");
}

void setRelay1(bool state) {
  relay1State = state;
  digitalWrite(RELAY1_PIN, state ? RELAY_ON : RELAY_OFF);
  Serial.println(state ? "AUTO Relay ON" : "AUTO Relay OFF");
}

void setRelay2(bool state) {
  relay2State = state;
  digitalWrite(RELAY2_PIN, state ? RELAY_ON : RELAY_OFF);
  Serial.println(state ? "OFF PHCN Relay ON" : "OFF PHCN Relay OFF");
}

void setRelay3(bool state) {
  relay3State = state;
  digitalWrite(RELAY3_PIN, state ? RELAY_ON : RELAY_OFF);
  Serial.println(state ? "OFF LOAD Relay ON" : "OFF LOAD Relay OFF");
}

// ============================================
// TIME
// ============================================

void initializeNTP() {
  Serial.println("Starting NTP time synchronization...");
  configTime(TIMEZONE_OFFSET, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
}

bool getCurrentDateKey(uint32_t &dateKey) {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    ntpTimeAvailable = false;
    return false;
  }
  struct tm timeInfo;
  localtime_r(&now, &timeInfo);
  dateKey = (timeInfo.tm_year + 1900) * 10000UL + (timeInfo.tm_mon + 1) * 100UL + timeInfo.tm_mday;
  ntpTimeAvailable = true;
  return true;
}

// Current time in UTC, e.g. 2026-11-23T17:30:00Z ("" if the clock is not set yet).
String isoNow() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";
  struct tm t;
  gmtime_r(&now, &t);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(buf);
}

// ============================================
// DAILY HISTORY QUEUE (LittleFS, unchanged idea)
// ============================================
// Completed days are queued and uploaded later, so a day that ended while
// Wi-Fi was down still reaches the web app's daily/weekly/monthly charts.

void queueDailyRecord(uint32_t date, uint32_t phcnSeconds, uint32_t genSeconds) {
  if (!littleFSAvailable) {
    Serial.println("LittleFS unavailable: daily record not queued.");
    return;
  }
  File file = LittleFS.open("/daily.csv", "a");
  if (!file) {
    Serial.println("Could not open daily history file.");
    return;
  }
  file.print(date);
  file.print(",");
  file.print(phcnSeconds);
  file.print(",");
  file.println(genSeconds);
  file.close();
  Serial.println("Daily record stored.");
}

void updateDailyRuntime(uint32_t addedPHCNSeconds, uint32_t addedGENSeconds) {
  uint32_t currentDate;
  if (!getCurrentDateKey(currentDate)) return;

  if (dailyDateKey == 0) {
    dailyDateKey = currentDate;
    dailyPHCNSeconds = 0;
    dailyGENSeconds = 0;
    saveRuntime();
    Serial.println("Daily tracking initialized.");
    return;
  }

  if (currentDate != dailyDateKey) {
    Serial.println("New day detected.");
    queueDailyRecord(dailyDateKey, dailyPHCNSeconds, dailyGENSeconds);
    dailyDateKey = currentDate;
    dailyPHCNSeconds = 0;
    dailyGENSeconds = 0;
    saveRuntime();
  }

  dailyPHCNSeconds += addedPHCNSeconds;
  dailyGENSeconds += addedGENSeconds;
}

// ============================================
// OPERATING HOURS (unchanged)
// ============================================

void updateRuntime() {
  uint32_t now = millis();
  uint32_t elapsed = now - lastRuntimeUpdate;
  lastRuntimeUpdate = now;

  bool phcnON = digitalRead(PHCN_SENSOR_PIN) == HIGH;
  bool genON = digitalRead(GEN_SENSOR_PIN) == HIGH;

  uint32_t addedPHCNSeconds = 0;
  uint32_t addedGENSeconds = 0;

  if (phcnON) {
    phcnRemainderMs += elapsed;
    addedPHCNSeconds = phcnRemainderMs / 1000UL;
    phcnRemainderMs %= 1000UL;
    phcnTotalSeconds += addedPHCNSeconds;
  }

  if (genON) {
    genRemainderMs += elapsed;
    addedGENSeconds = genRemainderMs / 1000UL;
    genRemainderMs %= 1000UL;
    genTotalSeconds += addedGENSeconds;
    genServiceSeconds += addedGENSeconds;
  }

  updateDailyRuntime(addedPHCNSeconds, addedGENSeconds);

  if (now - lastRuntimeSave >= SAVE_INTERVAL) {
    lastRuntimeSave = now;
    saveRuntime();
  }
}

void checkPowerSources() {
  int currentPHCN = digitalRead(PHCN_SENSOR_PIN);
  int currentGEN = digitalRead(GEN_SENSOR_PIN);
  phcnState = (currentPHCN == HIGH);
  genState = (currentGEN == HIGH);

  if (currentPHCN != lastPHCNState) {
    lastPHCNState = currentPHCN;
    Serial.print("PHCN: ");
    Serial.println(phcnState ? "AVAILABLE" : "NOT AVAILABLE");
    lastReport = 0; // report the change straight away
  }
  if (currentGEN != lastGENState) {
    lastGENState = currentGEN;
    Serial.print("GENERATOR: ");
    Serial.println(genState ? "AVAILABLE" : "NOT AVAILABLE");
    lastReport = 0;
  }
}

// ============================================
// WEB APP LINK (replaces Blynk)
// ============================================

// HTTPS request with this device's key. Returns the HTTP status code.
int callApp(const char* method, const String& path, const String& body, String& response) {
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();               // traffic is encrypted; certificate check skipped for simplicity
  client->setBufferSizes(4096, 1024);  // keeps memory use low on the ESP8266
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(*client, String(SERVER_URL) + path)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY);
  int code = (strcmp(method, "POST") == 0) ? http.POST(body) : http.GET();
  response = code > 0 ? http.getString() : "";
  http.end();
  return code;
}

String devicePath(const char* endpoint) {
  return String("/api/public/devices/") + DEVICE_ID + "/" + endpoint;
}

void sendAck(const String& id, const String& status, const String& reason) {
  JsonDocument doc;
  doc["commandId"] = id;
  doc["status"] = status;
  String at = isoNow();
  if (at.length()) doc["at"] = at;
  if (reason.length()) doc["reason"] = reason;
  String body, response;
  serializeJson(doc, body);
  int code = callApp("POST", devicePath("ack"), body, response);
  Serial.printf("Ack %s (%s) -> %d\n", id.c_str(), status.c_str(), code);
}

// Runs one command from the web app. Returns "success" or "rejected" (+ reason).
String executeCommand(const String& type, JsonObject payload, String& reason) {
  if (type == "auto_enable")         setRelay1(true);
  else if (type == "auto_disable")   setRelay1(false);
  else if (type == "phcn_relay_off") setRelay2(true);   // OFF PHCN relay engaged
  else if (type == "phcn_relay_on")  setRelay2(false);
  else if (type == "load_relay_off") setRelay3(true);   // OFF LOAD relay engaged
  else if (type == "load_relay_on")  setRelay3(false);

  else if (type == "reset_service_counter") {
    // Reset ONLY when service is due (same rule as before)
    if (genServiceSeconds < serviceLimitSeconds) {
      reason = "Generator service is not yet due";
      Serial.println("RESET DENIED: Generator service is not yet due.");
      return "rejected";
    }
    genServiceSeconds = 0;
    genRemainderMs = 0;
    serviceNotificationSent = false;
    saveRuntime();
    Serial.println("Generator service counter reset.");
  }

  else if (type == "set_service_interval") {
    float seconds = payload["intervalSeconds"] | 0.0f;
    float hours = seconds / 3600.0;
    if (hours < 1 || hours > MAX_SERVICE_HOURS) {
      reason = "Invalid service interval";
      return "rejected";
    }
    serviceLimitSeconds = (uint32_t)seconds;
    serviceNotificationSent = false;
    saveRuntime();
    Serial.printf("New service interval: %.1f hours.\n", hours);
  }

  else {
    reason = "Unknown command";
    return "rejected";
  }
  return "success";
}

void handleCommands(JsonArray commands) {
  for (JsonObject cmd : commands) {
    String id = cmd["id"].as<String>();
    String type = cmd["type"].as<String>();

    // Already done? Send the same answer again instead of switching twice.
    int seen = -1;
    for (int i = 0; i < 8; i++) if (doneIds[i] == id) seen = i;
    if (seen >= 0) {
      sendAck(id, doneStatus[seen], "");
      continue;
    }

    String reason;
    String status = executeCommand(type, cmd["payload"].as<JsonObject>(), reason);
    doneIds[doneNext] = id;
    doneStatus[doneNext] = status;
    doneNext = (doneNext + 1) % 8;
    sendAck(id, status, reason);
    lastReport = 0; // report the new relay state straight away
  }
}

// Quiet check-in: asks the web app for waiting commands without re-sending the
// readings. The app counts this as the board still being alive.
void pollCommands() {
  String body, response;
  int code = callApp("GET", devicePath("commands"), "", response);
  if (code != 200) {
    Serial.printf("Command poll failed: %d\n", code);
    if (code == 401) Serial.println("Check DEVICE_ID and DEVICE_KEY.");
    return;
  }
  JsonDocument reply;
  if (deserializeJson(reply, response)) return;
  handleCommands(reply["commands"].as<JsonArray>());
}

// Sends live readings; the reply contains any waiting commands.
void reportToApp() {
  JsonDocument doc;
  doc["phcn"] = phcnState;
  doc["generator"] = genState;
  doc["relay1_auto"] = relay1State;
  doc["relay2_off_phcn"] = relay2State;
  doc["relay3_off_load"] = relay3State;
  doc["phcn_total_s"] = phcnTotalSeconds;
  doc["gen_total_s"] = genTotalSeconds;
  doc["gen_service_s"] = genServiceSeconds;
  doc["daily_phcn_s"] = dailyPHCNSeconds > 86400UL ? 86400UL : dailyPHCNSeconds;
  doc["daily_gen_s"] = dailyGENSeconds > 86400UL ? 86400UL : dailyGENSeconds;
  doc["uptime_s"] = millis() / 1000UL;
  doc["rssi"] = WiFi.RSSI();
  doc["fw"] = FIRMWARE;
  String at = isoNow();
  if (at.length()) doc["at"] = at;

  String body, response;
  serializeJson(doc, body);
  int code = callApp("POST", devicePath("telemetry"), body, response);
  if (code != 200) {
    Serial.printf("Report failed: %d %s\n", code, response.c_str());
    if (code == 401) Serial.println("Check DEVICE_ID and DEVICE_KEY.");
    return;
  }

  JsonDocument reply;
  if (deserializeJson(reply, response)) return;
  handleCommands(reply["commands"].as<JsonArray>());
}

// Uploads one completed day from the queue (oldest first), like before.
// Sent as a frame time-stamped 23:59 Lagos on that day, which only updates that day's totals.
void sendNextPendingDailyRecord() {
  if (!littleFSAvailable || WiFi.status() != WL_CONNECTED) return;

  File input = LittleFS.open("/daily.csv", "r");
  if (!input || !input.available()) {
    if (input) input.close();
    return;
  }
  String line = input.readStringUntil('\n');
  input.close();

  uint32_t date = 0, phcnSeconds = 0, genSeconds = 0;
  int parsed = sscanf(line.c_str(), "%lu,%lu,%lu", &date, &phcnSeconds, &genSeconds);

  bool uploaded = (parsed != 3); // invalid lines are simply dropped
  if (parsed == 3) {
    char at[25];
    snprintf(at, sizeof(at), "%04lu-%02lu-%02luT22:59:00Z", date / 10000UL, (date / 100UL) % 100UL, date % 100UL);
    JsonDocument doc;
    doc["phcn"] = phcnState;
    doc["generator"] = genState;
    doc["relay1_auto"] = relay1State;
    doc["relay2_off_phcn"] = relay2State;
    doc["relay3_off_load"] = relay3State;
    doc["phcn_total_s"] = phcnTotalSeconds;
    doc["gen_total_s"] = genTotalSeconds;
    doc["gen_service_s"] = genServiceSeconds;
    doc["daily_phcn_s"] = phcnSeconds > 86400UL ? 86400UL : phcnSeconds;
    doc["daily_gen_s"] = genSeconds > 86400UL ? 86400UL : genSeconds;
    doc["uptime_s"] = millis() / 1000UL;
    doc["rssi"] = WiFi.RSSI();
    doc["fw"] = FIRMWARE;
    doc["at"] = at;
    String body, response;
    serializeJson(doc, body);
    int code = callApp("POST", devicePath("telemetry"), body, response);
    uploaded = (code == 200 || code == 400);
    Serial.printf("Uploaded daily record %lu -> %d\n", date, code);
  }
  if (!uploaded) return; // keep it for the next try

  // Remove the first line from the queue
  File in2 = LittleFS.open("/daily.csv", "r");
  File out = LittleFS.open("/daily.tmp", "w");
  if (!in2 || !out) {
    if (in2) in2.close();
    if (out) out.close();
    return;
  }
  in2.readStringUntil('\n');
  while (in2.available()) {
    String rest = in2.readStringUntil('\n');
    if (rest.length() > 0) out.println(rest);
  }
  in2.close();
  out.close();
  LittleFS.remove("/daily.csv");
  LittleFS.rename("/daily.tmp", "/daily.csv");
}

// ============================================
// SETUP
// ============================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("==============================");
  Serial.println("HASKE ATS CONTROLLER");
  Serial.println("Starting system...");
  Serial.println("==============================");

  setupPins();
  loadRuntime();

  littleFSAvailable = LittleFS.begin();
  Serial.println(littleFSAvailable ? "LittleFS initialized." : "WARNING: LittleFS initialization failed.");

  lastRuntimeUpdate = millis();
  lastRuntimeSave = millis();

  // Wi-Fi setup portal, same as before: join "Haske_ATS" to enter Wi-Fi details
  WiFiManager wifiManager;
  Serial.println("Connecting to WiFi...");
  if (!wifiManager.autoConnect("Haske_ATS")) {
    Serial.println("WiFi connection failed!");
    ESP.restart();
  }
  Serial.println("WiFi connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  initializeNTP();

  Serial.println("==============================");
  Serial.println("HASKE ATS READY");
  Serial.println("==============================");
}

// ============================================
// MAIN LOOP
// ============================================

uint32_t lastSecond = 0;

void loop() {
  uint32_t now = millis();

  if (now - lastSecond >= 1000UL) {
    lastSecond = now;
    checkPowerSources();
    updateRuntime();
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Full readings: straight after any change, then a heartbeat once a minute.
    if (lastReport == 0 || now - lastReport >= REPORT_INTERVAL) {
      lastReport = now == 0 ? 1 : now;
      lastCommandPoll = lastReport;
      reportToApp();
    } else if (now - lastCommandPoll >= COMMAND_POLL_INTERVAL) {
      // Quiet in-between check: commands only, no readings.
      lastCommandPoll = now;
      pollCommands();
    }
    if (now - lastHistory >= HISTORY_INTERVAL) {
      lastHistory = now;
      sendNextPendingDailyRecord();
    }
  }

  delay(10);
}
