// ============================================================
// EntGen Controller v0.2
//
// This code implements the EntGen controller.
// The function of this controller is to handle demand response
// commands. In the event of a demand response call, the 
// controller opens a relay to interrupt the grid voltage
// sensor on an ATS, causing the ATS to switch to the generator.
// If the generator fails to start or otherwise fails to 
// provide power to the load, the controller closes the relay
// and returns the load to the grid.
//
// Demand response signals may be received from the utility's
// DR management software or from the owner's phone app.
//
// The controller provides reporting and telemetry to both 
// the DR management software and to the phone app.
// ============================================================

#include <WiFi.h>
#include <WiFiServer.h>
#include <WiFiUdp.h>
#include <BlockDevice.h>
#include <LittleFileSystem.h>
#include <MBRBlockDevice.h>
#include <ArduinoRS485.h>
#include <ArduinoModbus.h>
#include <SPI.h>
#include <Ethernet.h>
#include <ArduinoMqttClient.h>
#include <ArduinoJson.h>
#include <mbed.h>

using namespace mbed;

// -- Configuration Section --------------------

// Filesystem
BlockDevice*      bd = BlockDevice::get_default_instance();
MBRBlockDevice    sys_bd(bd, 1);    // partition 1 - system/Wifi
MBRBlockDevice    usr_bd(bd, 2);    // partition 2 - user data
LittleFileSystem  fs("fs");

// Enrollment Configuration Structure

#define MAX_SSID_LEN        32
#define MAX_PASSWORD_LEN    64
#define MAX_URL_LEN         128
#define MAX_CLIENTID_LEN    64
#define MAX_SECRET_LEN      64
#define MAX_VENID_LEN       32
#define MAX_VENNAME_LEN     32
#define MAX_PROGRAMID_LEN   32
#define MAX_RESOURCES       16
#define MAX_RESOURCEID_LEN  32


#define CONFIG_FILE_PATH "/fs/config.json"
#define CONFIG_TEMP_PATH "/fs/config.json.tmp"
#define CONFIG_READ_BUFFER_SIZE 1536   // generous margin above expected file size

// This structure defines information in a resource.
// This information includes the resource name, and 
// can be expanded to include technical information
// such as rated power in the future. 
struct ResourceInfo {
  char name[MAX_RESOURCEID_LEN];
};

struct EnrollmentConfig {
  char          vtnURL[MAX_URL_LEN];
  char          clientId[MAX_CLIENTID_LEN];
  char          clientSecret[MAX_SECRET_LEN];

  char          wifiSSID[MAX_SSID_LEN];
  char          wifiPassword[MAX_PASSWORD_LEN];

  byte          mac[6];
  IPAddress     localIP_wifi;
  IPAddress     localIP_eth;
  IPAddress     gateway;
  IPAddress     subnet;
  IPAddress     dns;
  IPAddress     meterIP;       // meter IP address

  char          venId[MAX_VENID_LEN];
  char          venName[MAX_VENNAME_LEN];
  char          programId[MAX_PROGRAMID_LEN];
  ResourceInfo  resources[MAX_RESOURCES];
  uint8_t       resourceCount;
};

EnrollmentConfig registration;

struct ReconfigResult {
  bool ok;               // overall parse/validation success
  bool wifiChanged;
  bool mqttChanged;      // vtnURL/clientId/clientSecret
  bool identityChanged;  // venId/venName/programId
};

bool pendingWifiReconnect  = false;
bool pendingMqttReconnect  = false;

// Network configuration

// WiFi and Ethernet Configuration
WiFiClient wifiClient;
EthernetClient ethClient;

// Modbus Configuration
const int MODBUS_TCP_PORT = 502;
ModbusTCPClient modbusTCPClient(ethClient);
// Mode Selection - comment one out to select mode
#define USE_MODBUS_TCP
// #define USE_MODBUS_RTU

// MQTT Configuration
const int   BROKER_PORT = 1883;
MqttClient  mqttClient(wifiClient);
// MQTT Topics
#define MAX_TOPIC_LEN 64

struct TopicTable {
  char statusTopic[MAX_TOPIC_LEN];       // publish only -- status/LWT
  char appDispatchTopic[MAX_TOPIC_LEN];  // subscribe
  char appReconfigTopic[MAX_TOPIC_LEN];  // subscribe
  char reportTopic[MAX_TOPIC_LEN];       // publish only -- covers all report types, differentiated by report content
  char reportRequestTopic[MAX_TOPIC_LEN]; // subscribe
  // future: coopDispatchTopic, coopReconfigTopic, reportTopic, etc. --
  // added here as named fields when those channels actually exist, same
  // pattern as everything else in this struct.
};

TopicTable topics;

// Relay Configuration
bool dispatchGen = false;
bool prevDispatch = false;

// OpenADR Asset Configuration

// OpenADR JSON Templates Configuration
// OpenADR Command Template
struct OpenAdr3EventFields {
  const char* eventID;       // NEW -- event-level object identifier, doc[] scope
  const char* eventName;
  const char* programID;
  const char* period;
  const char* periodStart;
  const char* periodDur;
  const char* interval;
  const char* intervalId;    // interval[] scope -- same key name as eventID, different parent
  const char* payload;
  const char* payloadType;
  const char* payloadValue;
};

const OpenAdr3EventFields APP_COMMAND = {
  .eventID      = "id",
  .eventName    = "eventName",
  .programID    = "programID",
  .period       = "intervalPeriod",
  .periodStart  = "start",
  .periodDur    = "duration",
  .interval     = "intervals",
  .intervalId   = "id",
  .payload      = "payloads",
  .payloadType  = "type",
  .payloadValue = "values"
};

// OpenADR Report Template
struct OpenADR3ReportFields {
  const char* reportName;
  const char* clientName;
  const char* programID;
  const char* eventID;
  const char* resources;
  const char* resourceName;
  const char* interval;
  const char* intervalId;
  const char* payload;
  const char* payloadType;
  const char* payloadValue;
};

const OpenADR3ReportFields APP_REPORT = {
  .reportName     = "reportName",
  .clientName     = "clientName",
  .programID      = "programID",
  .eventID        = "eventID",
  .resources      = "resources",
  .resourceName   = "resourceName",
  .interval       = "intervals",
  .intervalId     = "id",
  .payload        = "payloads",
  .payloadType    = "type",
  .payloadValue   = "values"
};

// Sentinel values per OpenADR 3.0 §7.3 / §7.9
// Used to determine if a command is for an instant command or to clear a command
static const char* SENTINEL_START         = "0001-01-01T00:00:00";
static const char* SENTINEL_START_ALT     = "0001-01-01";
static const char* SENTINEL_DURATION_INF  = "P9999Y";
static const char* SENTINEL_DURATION_ZERO = "PT0S";

// Command Arbitration Configuration
// Lower number = higher precedence.
#define PRIORITY_APP      1
#define PRIORITY_COOP     50
#define PRIORITY_DEFAULT  99

#define MAX_EVENTID_LEN   32
#define MAX_EVENTNAME_LEN 32

// Parsed command template

enum CommandClassification { CMD_ASSERT, CMD_RELEASE, CMD_UNSUPPORTED_SCHEDULE };

struct ParsedCommand {
  char                  eventId[MAX_EVENTID_LEN];
  char                  eventName[MAX_EVENTNAME_LEN];
  uint8_t               dispatchValue;   // only meaningful when classification == CMD_ASSERT
  CommandClassification classification;
};

enum CommandSource { SOURCE_APP = 0, SOURCE_COOP = 1, SOURCE_SCHEDULE = 2, NUM_SOURCES = 3 };

struct CommandSlot {
  char          eventId[MAX_EVENTID_LEN];         // currently active event, if any
  char          eventName[MAX_EVENTNAME_LEN];
  uint8_t       dispatchValue;
  uint8_t       priority;
  bool          active;
  unsigned long receivedMillis;
  char          lastSeenEventId[MAX_EVENTID_LEN]; // most recent event ID from this source,
                                                    // regardless of whether it was actionable --
                                                    // traceability only, never read by arbitration
};

CommandSlot sources[NUM_SOURCES];

// Report Entry and Message Templates
#define MAX_REPORTNAME_LEN 32
#define MAX_CLIENTNAME_LEN 32
#define REPORT_BUFFER_SIZE 768   // generous margin for a handful of resources  

static const char* PAYLOAD_TYPE_SIMPLE = "SIMPLE";

// Resource template - one entry per resource included in a report
struct ReportResourceEntry {
  char    resourceName[MAX_RESOURCEID_LEN];
  bool    hasIntervalData;
  uint8_t payloadValue; // SIMPLE value, only meaningful if interval exists
};

struct ReportMessage {
  char                reportName[MAX_REPORTNAME_LEN];
  char                clientName[MAX_CLIENTNAME_LEN];
  bool                hasEventID;
  char                eventID[MAX_EVENTID_LEN];
  ReportResourceEntry resourceEntries[MAX_RESOURCES];
  uint8_t             resourceCount;
};


// -- Setup Section ----------------------------
// Code here runs once

void setup() {
  Serial.begin(115200);
  while (!Serial);
  Serial.println("Program has begun.");

  // Relay to a known-safe state FIRST, before any networking is attempted --
  // minimizes time spent in an undefined pin state while WiFi/Ethernet/MQTT
  // connection attempts are in progress.
  pinMode(D0, OUTPUT);
  pinMode(LED_D0, OUTPUT);
  digitalWrite(D0, LOW);
  digitalWrite(LED_D0, LOW);

  // Start File reading
  mountFilesystem();

  // Load the configuration file from memory

  if (!loadConfigFromFlash(registration)) {
  Serial.println("FATAL: No valid configuration found on flash.");
  Serial.println("This device may not have completed BLE commissioning.");
  Serial.println("Halting -- recommission via BLE before retrying.");
  while (1);
  }

  WiFi.config(registration.localIP_wifi, registration.dns, registration.gateway, registration.subnet);
  if (!connectWiFi()) {
    Serial.println("WiFi not connected at boot -- will keep retrying from loop().");
  }

  if (!connectEthernet()) {
    Serial.println("Ethernet not connected at boot -- will keep retrying from loop().");
  }

  #ifdef USE_MODBUS_TCP
    setupTCP();
  #endif
  #ifdef USE_MODBUS_RTU
    setupRTU();
  #endif

  if (connectMQTT()) {
    applyTopicSubscriptions();
  } else {
    Serial.println("MQTT not connected at boot -- will keep retrying from loop().");
  }

  initializeDefaultSource();
}


// --Main Program Loop-------------------------
// Code here runs continuously

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();   // return value not otherwise needed here -- next check below covers it
  }

  if (Ethernet.linkStatus() == LinkOFF) {
    connectEthernet();
  }

  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    if (connectMQTT()) {
      // Subscriptions don't survive a broker reconnect -- must be redone
      // every time a new MQTT session is established, not just at boot.
      applyTopicSubscriptions();
    }
  }

  #ifdef USE_MODBUS_TCP
    if (!modbusTCPClient.connected()) {
      setupTCP();
    }
  #endif
  #ifdef USE_MODBUS_RTU
    if (!modbusRTUClient.connected()) {
      setupRTU();
    }
  #endif

  mqttClient.poll();

  // -- Reconfiguration follow-up --------------------
  // Deferred from onMqttMessage()/applyReconfiguration(), rather than
  // reconnecting from inside the MQTT callback itself. Handled here, once
  // per loop pass, same as every other connection check above.
  if (pendingWifiReconnect) {
    pendingWifiReconnect = false;
    Serial.println("[RECONFIG] Applying new WiFi settings...");
    WiFi.config(registration.localIP_wifi, registration.dns, registration.gateway, registration.subnet);
    connectWiFi();
  }
  if (pendingMqttReconnect) {
    pendingMqttReconnect = false;
    Serial.println("[RECONFIG] Applying new MQTT/VTN settings...");
    mqttClient.stop();  // force a clean disconnect before reconnecting with new broker/credentials
    if (connectMQTT()) {
      applyTopicSubscriptions();
    }
  }

  resolveDispatchAuthority();
  updateRelayOutput();
}

// -- Filesystem Functions --------------------

// Filesystem mount
void mountFilesystem() {
  Serial.print("Initializing block device... ");
  int err = bd->init();
  if (err) {
    Serial.println("FAILED to initialize block device.");
    Serial.println("Halting.");
    while(1);
  }
  Serial.println("OK");

  // Create partition table if it doesn't exist
  // Partition 1: system/WiFi firmware - first 1MB
  // Partition 2: user data - remaining space
  Serial.print("Setting up partition table... ");
  err = MBRBlockDevice::partition(bd, 1, 0x83, 0, 1024 * 1024);
  if (err) {
    Serial.println("Partition 1 setup failed - may already exist, continuing...");
  }
  err = MBRBlockDevice::partition(bd, 2, 0x83, 1024 * 1024);
  if (err) {
    Serial.println("Partition 2 setup failed - may already exist, continuing...");
  }
  Serial.println("OK");

  Serial.print("Mounting filesystem... ");

  // Only mount partition 2 - never touch partition 1
  // Partition 1 contains WiFi firmware and must not be reformated
  err = fs.mount(&usr_bd);
  if (err) {
    Serial.println("not found. Formatting user partition only...");
    // Only reformat the user data partition, not the whole device
    err = fs.reformat(&usr_bd);
    if (err) {
      Serial.println("FAILED to format user partition.");
      Serial.println("Halting.");
      while(1);
    } else {
      Serial.println("OK");
    }
  } else {
    Serial.println("OK");
  }
}

// -- JSON Helper Functions -------------------

// Copies a JSON string field into a fixed-size destination buffer.
// Rejects (rather than silently truncates) values that don't fit.
bool copyJsonStringField(JsonVariantConst obj, const char* key,
                          char* dest, size_t destSize, bool required) {
  if (obj[key].isNull()) {
    if (required) {
      Serial.print("[CONFIG] Missing required field: ");
      Serial.println(key);
      return false;
    }
    dest[0] = '\0';
    return true;
  }

  const char* value = obj[key];
  if (value == nullptr) {
    Serial.print("[CONFIG] Field is not a string: ");
    Serial.println(key);
    return false;
  }

  size_t len = strlen(value);
  if (len >= destSize) {
    Serial.print("[CONFIG] Field too long, rejecting config: ");
    Serial.print(key);
    Serial.print(" (");
    Serial.print(len);
    Serial.print(" chars, max ");
    Serial.print(destSize - 1);
    Serial.println(")");
    return false;
  }

  strcpy(dest, value);  // length already validated above
  return true;
}

// Parses an IPAddress from a dotted-string JSON field, e.g. "10.0.0.101".
bool copyJsonIPField(JsonVariantConst obj, const char* key, IPAddress &dest, bool required) {
  if (obj[key].isNull()) {
    if (required) {
      Serial.print("[CONFIG] Missing required IP field: ");
      Serial.println(key);
      return false;
    }
    return true;
  }
  const char* value = obj[key];
  if (value == nullptr || !dest.fromString(value)) {
    Serial.print("[CONFIG] Invalid IP address for field: ");
    Serial.println(key);
    return false;
  }
  return true;
}

// Parses the MAC address from a JSON array of 6 integers, e.g. [0xDE, 0xAD, ...].
bool copyJsonMacField(JsonVariantConst obj, const char* key, byte* dest) {
  JsonArrayConst arr = obj[key].as<JsonArrayConst>();
  if (arr.isNull() || arr.size() != 6) {
    Serial.print("[CONFIG] Missing or malformed MAC address field: ");
    Serial.println(key);
    return false;
  }
  int i = 0;
  for (JsonVariantConst v : arr) {
    dest[i++] = (byte)v.as<int>();
  }
  return true;
}

// -- Enrollment Functions --------------------

// Load configuration file
// Reads persisted config from LittleFS into the given struct.
bool loadConfigFromFlash(EnrollmentConfig &cfg) {
  FILE* f = fopen(CONFIG_FILE_PATH, "r");
  if (f == nullptr) {
    Serial.print("[CONFIG] Could not open ");
    Serial.println(CONFIG_FILE_PATH);
    return false;
  }

  static char buffer[CONFIG_READ_BUFFER_SIZE];
  size_t bytesRead = fread(buffer, 1, CONFIG_READ_BUFFER_SIZE - 1, f);
  fclose(f);

  if (bytesRead == 0) {
    Serial.println("[CONFIG] Config file is empty or unreadable.");
    return false;
  }
  buffer[bytesRead] = '\0';

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, buffer, bytesRead);
  if (err) {
    Serial.print("[CONFIG] JSON parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  bool ok = true;
  ok &= copyJsonStringField(doc, "vtnURL",       cfg.vtnURL,       MAX_URL_LEN,      true);
  ok &= copyJsonStringField(doc, "clientId",     cfg.clientId,     MAX_CLIENTID_LEN, true);
  ok &= copyJsonStringField(doc, "clientSecret", cfg.clientSecret, MAX_SECRET_LEN,   true);
  ok &= copyJsonStringField(doc, "wifiSSID",     cfg.wifiSSID,     MAX_SSID_LEN,     true);
  ok &= copyJsonStringField(doc, "wifiPassword", cfg.wifiPassword, MAX_PASSWORD_LEN, true);
  ok &= copyJsonStringField(doc, "venId",        cfg.venId,        MAX_VENID_LEN,    true);
  ok &= copyJsonStringField(doc, "venName",      cfg.venName,      MAX_VENNAME_LEN,  true);
  ok &= copyJsonStringField(doc, "programId",    cfg.programId,    MAX_PROGRAMID_LEN, true);

  ok &= copyJsonMacField(doc, "mac", cfg.mac);

  // IPAddress -> dotted-decimal string
  ok &= copyJsonIPField(doc, "localIP_wifi", cfg.localIP_wifi, true);
  ok &= copyJsonIPField(doc, "localIP_eth",  cfg.localIP_eth,  true);
  ok &= copyJsonIPField(doc, "gateway",      cfg.gateway,      true);
  ok &= copyJsonIPField(doc, "subnet",       cfg.subnet,       true);
  ok &= copyJsonIPField(doc, "dns",          cfg.dns,          true);
  ok &= copyJsonIPField(doc, "meterIP",      cfg.meterIP,      true);

  JsonVariantConst topicsObj = doc["topics"];
  if (topicsObj.isNull()) {
    Serial.println("[CONFIG] Missing 'topics' section, rejecting config.");
    ok = false;
  } else {
    ok &= copyJsonStringField(topicsObj, "statusTopic",         topics.statusTopic,         MAX_TOPIC_LEN, true);
    ok &= copyJsonStringField(topicsObj, "appDispatchTopic",    topics.appDispatchTopic,    MAX_TOPIC_LEN, true);
    ok &= copyJsonStringField(topicsObj, "appReconfigTopic",    topics.appReconfigTopic,    MAX_TOPIC_LEN, true);
    ok &= copyJsonStringField(topicsObj, "reportTopic",         topics.reportTopic,         MAX_TOPIC_LEN, true);
    ok &= copyJsonStringField(topicsObj, "reportRequestTopic",  topics.reportRequestTopic,  MAX_TOPIC_LEN, true);
  }

  // Resources: a required, possibly-empty JSON array. 
  JsonVariantConst resourcesArr = doc["resources"];
  if (!resourcesArr.is<JsonArrayConst>()) {
    Serial.println("[CONFIG] Missing or malformed 'resources' array, rejecting config.");
    ok = false;
  } else {
    uint8_t count = 0;
    for (JsonVariantConst entry : resourcesArr.as<JsonArrayConst>()) {
      if (count >= MAX_RESOURCES) {
        Serial.println("[CONFIG] WARNING: more resources in file than MAX_RESOURCES, truncating.");
        break;
      }
      if (!copyJsonStringField(entry, "name", cfg.resources[count].name, MAX_RESOURCEID_LEN, true)) {
        ok = false;
        break;
      }
      count++;
    }
    cfg.resourceCount = count;
  }

  if (!ok) {
    Serial.println("[CONFIG] One or more fields failed validation, rejecting config.");
    return false;
  }

  Serial.println("[CONFIG] Configuration loaded successfully.");
  return true;
}

// Save configuration file
// Writes an updated config from the given struct into LittleFS
// Writes to a temporary file to start, then overwrites the permanent
// config file once the full file has been confirmed. This avoids having
// an invalid truncated file if the write fails part of the way through.
bool saveConfigToFlash(const EnrollmentConfig &cfg) {
  JsonDocument doc;

  doc["vtnURL"]       = cfg.vtnURL;
  doc["clientId"]     = cfg.clientId;
  doc["clientSecret"] = cfg.clientSecret;
  doc["wifiSSID"]     = cfg.wifiSSID;
  doc["wifiPassword"] = cfg.wifiPassword;
  doc["venId"]        = cfg.venId;
  doc["venName"]      = cfg.venName;
  doc["programId"]    = cfg.programId;

  // IPAddress -> dotted-decimal string
  doc["localIP_wifi"] = cfg.localIP_wifi.toString();
  doc["localIP_eth"]  = cfg.localIP_eth.toString();
  doc["gateway"]      = cfg.gateway.toString();
  doc["subnet"]       = cfg.subnet.toString();
  doc["dns"]          = cfg.dns.toString();
  doc["meterIP"]      = cfg.meterIP.toString();

  JsonArray macArr = doc["mac"].to<JsonArray>();
  for (int i = 0; i < 6; i++) {
    macArr.add(cfg.mac[i]);
  }

  JsonObject topicsObj = doc["topics"].to<JsonObject>();
  topicsObj["statusTopic"]      =   topics.statusTopic;
  topicsObj["appDispatchTopic"] =   topics.appDispatchTopic;
  topicsObj["appReconfigTopic"] =   topics.appReconfigTopic;
  topicsObj["reportTopic"] =        topics.reportTopic;
  topicsObj["reportRequestTopic"] = topics.reportRequestTopic;

  JsonArray resourcesArr = doc["resources"].to<JsonArray>();
  for (uint8_t i = 0; i < cfg.resourceCount; i++) {
    JsonObject resourceObj = resourcesArr.add<JsonObject>();
    resourceObj["name"] = cfg.resources[i].name;
  }

  FILE* f = fopen(CONFIG_TEMP_PATH, "w");
  if (f == nullptr) {
    Serial.print("[CONFIG] Could not open temp file for writing: ");
    Serial.println(CONFIG_TEMP_PATH);
    return false;
  }

  static char buffer[CONFIG_READ_BUFFER_SIZE];
  size_t written = serializeJson(doc, buffer, CONFIG_READ_BUFFER_SIZE);
  if (written == 0 || written >= CONFIG_READ_BUFFER_SIZE) {
    Serial.println("[CONFIG] Serialized config too large for buffer, aborting save.");
    fclose(f);
    remove(CONFIG_TEMP_PATH);
    return false;
  }

  size_t writtenToFile = fwrite(buffer, 1, written, f);
  fclose(f);

  if (writtenToFile != written) {
    Serial.println("[CONFIG] Incomplete write to temp file, aborting save.");
    remove(CONFIG_TEMP_PATH);
    return false;
  }

  // Atomic-ish swap: POSIX rename() replaces the destination file in a
  // single filesystem operation rather than a read-modify-write on the
  // original -- this is the step that actually protects against a
  // corrupted config.json if power is lost mid-save.
  if (rename(CONFIG_TEMP_PATH, CONFIG_FILE_PATH) != 0) {
    Serial.println("[CONFIG] Failed to rename temp file into place.");
    remove(CONFIG_TEMP_PATH);
    return false;
  }

  Serial.println("[CONFIG] Configuration saved successfully.");
  return true;
}

// Reconfigure function
// Applies a live reconfiguration payload (from the reconfig MQTT topic)
// to the in-memory config, persists it, and reports what changed so the
// caller can decide what needs to reconnect.
ReconfigResult applyReconfiguration(const char* jsonPayload, EnrollmentConfig &cfg) {
  ReconfigResult result = { false, false, false, false };

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, jsonPayload);
  if (err) {
    Serial.print("[RECONFIG] JSON parse error: ");
    Serial.println(err.c_str());
    return result;
  }

  EnrollmentConfig updated = cfg;  // start from current values; only touch what's present
  bool valid = true;

  if (!doc["wifiSSID"].isNull())     valid &= copyJsonStringField(doc, "wifiSSID", updated.wifiSSID, MAX_SSID_LEN, false);
  if (!doc["wifiPassword"].isNull()) valid &= copyJsonStringField(doc, "wifiPassword", updated.wifiPassword, MAX_PASSWORD_LEN, false);
  if (!doc["localIP_wifi"].isNull()) valid &= copyJsonIPField(doc, "localIP_wifi", updated.localIP_wifi, false);
  if (!doc["gateway"].isNull())      valid &= copyJsonIPField(doc, "gateway", updated.gateway, false);
  if (!doc["subnet"].isNull())       valid &= copyJsonIPField(doc, "subnet", updated.subnet, false);
  if (!doc["dns"].isNull())          valid &= copyJsonIPField(doc, "dns", updated.dns, false);

  if (!doc["vtnURL"].isNull())       valid &= copyJsonStringField(doc, "vtnURL", updated.vtnURL, MAX_URL_LEN, false);
  if (!doc["clientId"].isNull())     valid &= copyJsonStringField(doc, "clientId", updated.clientId, MAX_CLIENTID_LEN, false);
  if (!doc["clientSecret"].isNull()) valid &= copyJsonStringField(doc, "clientSecret", updated.clientSecret, MAX_SECRET_LEN, false);

  if (!doc["venId"].isNull())        valid &= copyJsonStringField(doc, "venId", updated.venId, MAX_VENID_LEN, false);
  if (!doc["venName"].isNull())      valid &= copyJsonStringField(doc, "venName", updated.venName, MAX_VENNAME_LEN, false);
  if (!doc["programId"].isNull())    valid &= copyJsonStringField(doc, "programId", updated.programId, MAX_PROGRAMID_LEN, false);

  if (!valid) {
    Serial.println("[RECONFIG] One or more fields failed validation, rejecting reconfiguration.");
    return result;  // cfg left untouched
  }

  // Diff against the ORIGINAL cfg, before it gets overwritten below.
  result.wifiChanged = (strcmp(cfg.wifiSSID, updated.wifiSSID) != 0) ||
                       (strcmp(cfg.wifiPassword, updated.wifiPassword) != 0) ||
                       (cfg.localIP_wifi != updated.localIP_wifi) ||
                       (cfg.gateway != updated.gateway) ||
                       (cfg.subnet != updated.subnet) ||
                       (cfg.dns != updated.dns);

  result.mqttChanged = (strcmp(cfg.vtnURL, updated.vtnURL) != 0) ||
                       (strcmp(cfg.clientId, updated.clientId) != 0) ||
                       (strcmp(cfg.clientSecret, updated.clientSecret) != 0);

  result.identityChanged = (strcmp(cfg.venId, updated.venId) != 0) ||
                           (strcmp(cfg.venName, updated.venName) != 0) ||
                           (strcmp(cfg.programId, updated.programId) != 0);

  cfg = updated;

  if (!saveConfigToFlash(cfg)) {
    // Applied in RAM but not persisted -- device runs correctly now, but
    // would revert to old values on next reboot unless this is resolved.
    // Not treated as a failed reconfiguration; surfaced loudly instead.
    Serial.println("[RECONFIG] WARNING: applied in memory but FAILED to persist to flash.");
    Serial.println("[RECONFIG] Changes will be lost on next reboot unless reapplied.");
  }

  if (result.identityChanged) {
    // venId/programId changing invalidates whatever app/coop commands were
    // validated against the OLD programId -- per earlier design decision,
    // treat this as a soft reset of arbitration state rather than letting
    // stale commands keep applying under a changed identity.
    Serial.println("[RECONFIG] Identity changed -- releasing all active app/coop commands.");
    resetCommandSlot(sources[SOURCE_APP],  PRIORITY_APP);
    resetCommandSlot(sources[SOURCE_COOP], PRIORITY_COOP);
    resolveDispatchAuthority();
    updateRelayOutput();
  }

  result.ok = true;
  Serial.println("[RECONFIG] Reconfiguration applied successfully.");
  return result;
}

// -- WiFi and Ethernet Functions -------------

// WiFi connection
// Makes 10 attempts to connect to the WiFi.
// Provides troubleshooting for the connection if it fails.
bool connectWiFi() {
  WiFi.disconnect();
  WiFi.end();
  delay(1000);
  Serial.print("Connecting to WiFi");
  WiFi.begin(registration.wifiSSID, registration.wifiPassword);

  int attempts = 0;
  const int MAX_ATTEMPTS = 10;

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;

    if (attempts >= MAX_ATTEMPTS) {
      Serial.println();
      Serial.print("WiFi connection failed after ");
      Serial.print(MAX_ATTEMPTS / 2);
      Serial.println(" seconds. Status code: ");
      switch (WiFi.status()) {
        case WL_DISCONNECTED:
          Serial.println("  WL_DISCONNECTED — check SSID and password");
          break;
        case WL_CONNECTION_LOST:
          Serial.println("  WL_CONNECTION_LOST — signal may be too weak");
          break;
        case WL_CONNECT_FAILED:
          Serial.println("  WL_CONNECT_FAILED — authentication failed, check password");
          break;
        case WL_NO_SSID_AVAIL:
          Serial.println("  WL_NO_SSID_AVAIL — network not found, check SSID");
          break;
        case WL_IDLE_STATUS:
          Serial.println("  WL_IDLE_STATUS — WiFi module not responding");
          break;
        default:
          Serial.print("  Unknown status code: ");
          Serial.println(WiFi.status());
          break;
      }
      Serial.println("Giving up for now -- will retry on next loop() pass.");
      return false;   // bounded -- always returns, never halts
    }
  }

  Serial.println();
  Serial.print("WiFi connected to: ");
  Serial.println(registration.wifiSSID);

  if (WiFi.localIP() == registration.localIP_wifi) {
    Serial.print("Static IP configured successfully. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.print("Warning: IP mismatch. Got: ");
    Serial.println(WiFi.localIP());
    Serial.print("Expected: ");
    Serial.println(registration.localIP_wifi);
    Serial.println("Continuing with assigned IP — update localIP in sketch if needed.");
  }
  return true;
}

// Ethernet connection
// Makes 10 attempts to connect to the Ethernet
// Provides troubleshooting for the connection if it fails
bool connectEthernet() {
  Serial.print("Connecting to Ethernet");
  Ethernet.begin(registration.mac, registration.localIP_eth, registration.dns, registration.gateway, registration.subnet);

  int attempts = 0;
  const int MAX_ATTEMPTS = 10;

  switch (Ethernet.hardwareStatus()) {
    case EthernetNoHardware:
      Serial.println("  EthernetNoHardware - controller not connected, check board/wiring");
      return false;

    case EthernetMbed: {
      while (Ethernet.linkStatus() == LinkOFF) {
        delay(500);
        Serial.print(".");
        attempts++;

        if (attempts >= MAX_ATTEMPTS) {
          Serial.println();
          Serial.print("Ethernet connection failed after ");
          Serial.print(MAX_ATTEMPTS / 2);
          Serial.println(" seconds. Status code: ");
          switch (Ethernet.linkStatus()) {
            case LinkOFF:
              Serial.println("  LinkOFF — check cable is plugged in at both ends");
              break;
            case LinkON:
              Serial.println("  LinkON but no IP — check DHCP server, or switch to static IP");
              break;
            case Unknown:
            default:
              Serial.println("  Unknown link status — board may not support link detection");
              break;
          }
          return false;
        }
      }
      break;
    }

    default:
      Serial.print("  Unrecognized hardwareStatus() code: ");
      Serial.println(Ethernet.hardwareStatus());
      return false;
  }

  Serial.println(" Link up.");
  delay(500);

  if (Ethernet.localIP() == registration.localIP_eth) {
    Serial.print("Static IP configured successfully. IP: ");
    Serial.println(Ethernet.localIP());
  } else {
    Serial.print("Warning: IP mismatch. Got: ");
    Serial.println(Ethernet.localIP());
    Serial.print("Expected: ");
    Serial.println(registration.localIP_eth);
    Serial.println("Continuing with assigned IP — update localIP_eth via reconfiguration if needed.");
  }
  return true;
}

// -- Modbus Functions ------------------------

// TCP Setup
void setupTCP() {
  if (!modbusTCPClient.begin(registration.meterIP, MODBUS_TCP_PORT)) {
    Serial.print("Failed to connect to Modbus TCP server. Error: ");
    Serial.println(modbusTCPClient.lastError());
    return;
  }
  Serial.println("Modbus TCP connected.");
}

// RTU Setup
void setupRTU() {
  // Default device address 1, 9600 baud, 8N1
  if (!ModbusRTUClient.begin(9600)) {
    Serial.print("Failed to start Modbus RTU. Error: ");
    Serial.println(ModbusRTUClient.lastError());
    return;
  }
  Serial.println("Modbus RTU started.");
}

// Reading an input register
int readInputRegister(int index) {
  #ifdef USE_MODBUS_TCP
    int val = modbusTCPClient.inputRegisterRead(index);
    if (val < 0) {
      Serial.print("IR read error on index ");
      Serial.print(index);
      Serial.print(" — error code: ");
      Serial.println(modbusTCPClient.lastError());
    }
    return val;
  #endif
  #ifdef USE_MODBUS_RTU
    int val = ModbusRTUClient.inputRegisterRead(1, index);
    if (val < 0) {
      Serial.print("IR read error on index ");
      Serial.print(index);
      Serial.print(" — error code: ");
      Serial.println(ModbusRTUClient.lastError());
    }
    return val;
  #endif
}

// Reading a holding register
int readHoldingRegister(int index) {
  #ifdef USE_MODBUS_TCP
    int val = modbusTCPClient.holdingRegisterRead(index);
    if (val < 0) {
      Serial.print("HR read error on index ");
      Serial.print(index);
      Serial.print(" — error code: ");
      Serial.println(modbusTCPClient.lastError());
    }
    return val;
  #endif
  #ifdef USE_MODBUS_RTU
    int val = ModbusRTUClient.holdingRegisterRead(1, index);
    if (val < 0) {
      Serial.print("HR read error on index ");
      Serial.print(index);
      Serial.print(" — error code: ");
      Serial.println(modbusRTUClient.lastError());
    }
    return val;
  #endif
}

// Writing a holding register
void writeHoldingRegister(int index, int value) {
  #ifdef USE_MODBUS_TCP
    modbusTCPClient.holdingRegisterWrite(index, value);
  #endif
  #ifdef USE_MODBUS_RTU
    ModbusRTUClient.holdingRegisterWrite(1, index, value);
  #endif
}

// -- MQTT Functions --------------------------

bool connectMQTT() {
  mqttClient.beginWill(topics.statusTopic, true, 1);
  mqttClient.print("offline");
  mqttClient.endWill();

  Serial.print("Connecting to MQTT broker");
  int attempts = 0;
  const int MAX_ATTEMPTS = 10;

  while (!mqttClient.connect(registration.vtnURL, BROKER_PORT)) {
    int error = mqttClient.connectError();
    Serial.println();
    Serial.print("Connection attempt ");
    Serial.print(attempts + 1);
    Serial.print(" failed. Error code: ");
    Serial.print(error);
    Serial.print(" — ");
    switch (error) {
      case -2: Serial.println("Connection refused"); break;
      case -1: Serial.println("Connection timeout");  break;
      case  1: Serial.println("Unacceptable protocol version"); break;
      case  2: Serial.println("Client ID rejected"); break;
      case  3: Serial.println("Server unavailable"); break;
      case  4: Serial.println("Bad username or password"); break;
      case  5: Serial.println("Not authorized"); break;
      default: Serial.println("Unknown error"); break;
    }

    attempts++;
    if (attempts >= MAX_ATTEMPTS) {
      Serial.println("Max attempts reached -- will retry on next loop() pass.");
      return false;
    }
    delay(500);
  }
  Serial.println();
  Serial.println("MQTT connected.");

  mqttClient.beginMessage(topics.statusTopic, true);
  mqttClient.print("online");
  mqttClient.endMessage();
  Serial.println("Announced presence by setting presence to online");
  return true;
}

// MQTT Subscription function
void applyTopicSubscriptions() {
  mqttClient.subscribe(topics.appDispatchTopic, 1);
  mqttClient.subscribe(topics.appReconfigTopic, 1);
  mqttClient.subscribe(topics.reportRequestTopic, 1);

  mqttClient.onMessage(onMqttMessage);
  Serial.println("[MQTT] Subscriptions applied.");

  // Inform the VTN that the VEN has been provisioned and enrolled
  publishEnrollmentReport();
}

// MQTT Message handler
void onMqttMessage(int messageSize) {
  String topic = mqttClient.messageTopic();

  String payloadStr;
  payloadStr.reserve(messageSize);
  while (mqttClient.available()) {
    payloadStr += (char)mqttClient.read();
  }

  Serial.print("[MQTT] Message on "); Serial.print(topic);
  Serial.print(" ("); Serial.print(messageSize); Serial.println(" bytes)");

  if (topic == topics.appDispatchTopic) {
    parseOpenADR3Dispatch(payloadStr.c_str(), APP_COMMAND, registration.programId, SOURCE_APP);
    return;
  } 
  if (topic == topics.appReconfigTopic) {
    ReconfigResult r = applyReconfiguration(payloadStr.c_str(), registration);
    if (r.ok) {
      pendingWifiReconnect |= r.wifiChanged;
      pendingMqttReconnect |= r.mqttChanged;
    }
    return;
  }
  if (topic == topics.reportRequestTopic) {
    parseReportRequest(payloadStr.c_str());
    return;
  } 
  Serial.print("[MQTT] No handler registered for topic: ");
  Serial.println(topic);
}

// -- Relay Control Functions -----------------

// Update the relay output based on dispatchGen command.
void updateRelayOutput() {
  if (dispatchGen && !prevDispatch) {
    Serial.println("Dispatching backup generator.");
    digitalWrite(D0, HIGH);
    digitalWrite(LED_D0, HIGH);
  } else if (!dispatchGen && prevDispatch) {
    Serial.println("Returning to grid power.");
    digitalWrite(D0, LOW);
    digitalWrite(LED_D0, LOW);
  }
  prevDispatch = dispatchGen;
}

// -- Control Source Functions ----------------

void resetCommandSlot(CommandSlot &slot, uint8_t priority) {
  slot.eventId[0]         = '\0';
  slot.eventName[0]       = '\0';
  slot.dispatchValue      = 0;
  slot.priority           = priority;
  slot.active             = false;
  slot.receivedMillis     = 0;
  slot.lastSeenEventId[0] = '\0';
}

void initializeDefaultSource() {
  CommandSlot &def = sources[SOURCE_SCHEDULE];

  strncpy(def.eventId, "default", MAX_EVENTID_LEN - 1);
  def.eventId[MAX_EVENTID_LEN - 1] = '\0';
  strncpy(def.eventName, "default", MAX_EVENTNAME_LEN - 1);
  def.eventName[MAX_EVENTNAME_LEN - 1] = '\0';
  def.dispatchValue  = 0;
  def.priority       = PRIORITY_DEFAULT;
  def.active         = true;
  def.receivedMillis = millis();

  resetCommandSlot(sources[SOURCE_APP],  PRIORITY_APP);
  resetCommandSlot(sources[SOURCE_COOP], PRIORITY_COOP);

  Serial.println("[SCHED] Default source initialized: priority=99, value=grid, active");
}

// -- App Control Functions -------------------

void parseOpenADR3Dispatch(const char* payloadStr, const OpenAdr3EventFields& fields,
                            const char* regProgram, CommandSource source) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payloadStr);
  if (err) {
    Serial.print("[PARSE] JSON error: ");
    Serial.println(err.c_str());
    return;
  }

  if (doc[fields.programID].isNull() || doc[fields.interval].isNull() || doc[fields.eventID].isNull()) {
    Serial.println("[PARSE] Missing required top-level field(s), rejecting event");
    return;
  }

  const char* programID = doc[fields.programID];
  if (strcmp(programID, regProgram) != 0) {
    Serial.print("[PARSE] Asset is not registered for program ");
    Serial.print(programID);
    Serial.println(", rejecting event.");
    return;
  }

  const char* eventId   = doc[fields.eventID];
  const char* eventName = doc[fields.eventName];

  JsonArray intervals = doc[fields.interval].as<JsonArray>();
  if (intervals.size() == 0) {
    Serial.println("[PARSE] No intervals present, rejecting event");
    return;
  }

  JsonObject interval0 = intervals[0];
  if (interval0[fields.intervalId].isNull() || interval0[fields.payload].isNull()) {
    Serial.println("[PARSE] Interval missing id or payloads, rejecting event.");
    return;
  }

  const char* startStr;
  const char* durStr;
  bool hasOwnPeriod = interval0[fields.period].is<JsonObject>();
  if (hasOwnPeriod) {
    startStr = interval0[fields.period][fields.periodStart];
    durStr   = interval0[fields.period][fields.periodDur];
  } else if (doc[fields.period].is<JsonObject>()) {
    startStr = doc[fields.period][fields.periodStart];
    durStr   = doc[fields.period][fields.periodDur];
  } else {
    Serial.println("[PARSE] No intervalPeriod at event or interval level, rejecting event.");
    return;
  }

  if (startStr == nullptr || durStr == nullptr) {
    Serial.println("[PARSE] Missing start or duration, rejecting event.");
    return;
  }

  bool isImmediateStart = (strcmp(startStr, SENTINEL_START) == 0) ||
                          (strcmp(startStr, SENTINEL_START_ALT) == 0);
  bool isIndefiniteDur  = (strcmp(durStr, SENTINEL_DURATION_INF) == 0);
  bool isZeroDur        = (strcmp(durStr, SENTINEL_DURATION_ZERO) == 0);

  ParsedCommand cmd = {};
  strncpy(cmd.eventId, eventId, MAX_EVENTID_LEN - 1);
  cmd.eventId[MAX_EVENTID_LEN - 1] = '\0';
  strncpy(cmd.eventName, eventName ? eventName : "", MAX_EVENTNAME_LEN - 1);
  cmd.eventName[MAX_EVENTNAME_LEN - 1] = '\0';

  if (isImmediateStart && isIndefiniteDur) {
    JsonArray payloads = interval0[fields.payload].as<JsonArray>();
    if (payloads.size() == 0) {
      Serial.println("[PARSE] No payloads in interval, rejecting event");
      return;
    }
    JsonVariant rawValue = payloads[0][fields.payloadValue][0];
    if (rawValue.isNull()) {
      Serial.println("[PARSE] No dispatch value in payload, rejecting event");
      return;
    }
    int value = rawValue.as<int>();
    if (value != 0 && value != 1) {
      Serial.print("[PARSE] Unrecognized dispatch value: ");
      Serial.print(value);
      Serial.println(", rejecting event");
      return;
    }
    cmd.dispatchValue  = (uint8_t)value;
    cmd.classification = CMD_ASSERT;

  } else if (isImmediateStart && isZeroDur) {
    cmd.dispatchValue  = 0;  // unused for release, per earlier design
    cmd.classification = CMD_RELEASE;

  } else {
    cmd.dispatchValue  = 0;  // unused for unsupported-schedule
    cmd.classification = CMD_UNSUPPORTED_SCHEDULE;
    Serial.println("[PARSE] Scheduled events are not supported by this controller yet.");
  }

  handleParsedCommand(cmd, source);
}

// Update Command Slot
// Updates the given source's stored slot based on a parsed/classified
// command, then re-resolves overall dispatch authority from scratch.
// Sources may be default, App, Coop, or scheduler per current design.
void handleParsedCommand(const ParsedCommand &cmd, CommandSource source) {
  CommandSlot &slot = sources[source];

  // Traceability record, independent of whether this command is actionable.
  strncpy(slot.lastSeenEventId, cmd.eventId, MAX_EVENTID_LEN - 1);
  slot.lastSeenEventId[MAX_EVENTID_LEN - 1] = '\0';

  switch (cmd.classification) {
    case CMD_ASSERT: {
      uint8_t fixedPriority = (source == SOURCE_APP) ? PRIORITY_APP
                            : (source == SOURCE_COOP) ? PRIORITY_COOP
                            : PRIORITY_DEFAULT;

      strncpy(slot.eventId, cmd.eventId, MAX_EVENTID_LEN - 1);
      slot.eventId[MAX_EVENTID_LEN - 1] = '\0';
      strncpy(slot.eventName, cmd.eventName, MAX_EVENTNAME_LEN - 1);
      slot.eventName[MAX_EVENTNAME_LEN - 1] = '\0';
      slot.dispatchValue  = cmd.dispatchValue;
      slot.priority       = fixedPriority;
      slot.active         = true;
      slot.receivedMillis = millis();

      Serial.print("[COMMAND] Source "); Serial.print(source);
      Serial.print(" asserted event '"); Serial.print(cmd.eventId);
      Serial.print("' value="); Serial.println(cmd.dispatchValue);

      publishEventStatusReport(source);
      break;
    }

    case CMD_RELEASE: {
      if (!slot.active) {
        Serial.print("[COMMAND] Release received for source "); Serial.print(source);
        Serial.println(" but no command is currently active, ignoring.");
        break;
      }
      if (strcmp(slot.eventId, cmd.eventId) != 0) {
        Serial.print("[COMMAND] Release for event '"); Serial.print(cmd.eventId);
        Serial.print("' does not match currently active event '");
        Serial.print(slot.eventId); Serial.println("', ignoring stale release.");
        break;
      }
      Serial.print("[COMMAND] Releasing event '"); Serial.print(cmd.eventId);
      Serial.print("' from source "); Serial.println(source);
      slot.active     = false;
      slot.eventId[0] = '\0';
      break;
    }

    case CMD_UNSUPPORTED_SCHEDULE: {
      Serial.print("[COMMAND] Unsupported scheduled event '"); Serial.print(cmd.eventId);
      Serial.print("' from source "); Serial.print(source);
      Serial.println(" logged, not acted upon.");
      // Deliberately does not touch slot.active/eventId/dispatchValue/priority --
      // this source's arbitration state is untouched by a command we can't act on.
      break;
    }
  }
}

// Select active dispatch command
// Full rescan across all sources, picking the lowest priority *number*
// (highest precedence) among currently active sources. 
// Ties: first match in iteration order (SOURCE_APP, SOURCE_COOP,
// SOURCE_SCHEDULE) wins -- i.e., app beats coop beats schedule on a tie.
void resolveDispatchAuthority() {
  int winner = -1;
  uint8_t bestPriority = 255;

  for (int i = 0; i < NUM_SOURCES; i++) {
    if (sources[i].active && sources[i].priority < bestPriority) {
      bestPriority = sources[i].priority;
      winner = i;
    }
  }

  // winner should never be -1 given SOURCE_SCHEDULE's permanent active
  // state -- treat as a bug, not a runtime condition, if this ever fires.
  if (winner == -1) {
    Serial.println("[COMMAND] ERROR: no active source found during arbitration -- this should be unreachable.");
    dispatchGen = false;
    return;
  }

  dispatchGen = (sources[winner].dispatchValue == 1);

  Serial.print("[COMMAND] Arbitration winner: source "); Serial.print(winner);
  Serial.print(" (priority "); Serial.print(sources[winner].priority);
  Serial.print(") -> dispatchGen="); Serial.println(dispatchGen ? "true" : "false");
}

// -- App Reporting Functions -----------------

// Used to build an enrollment report initiated by the Opta
void publishEnrollmentReport() {
  ReportMessage report;
  if (!buildEnrollmentReport(report)) {
    return;
  }

  static char reportBuffer[REPORT_BUFFER_SIZE];
  size_t written;
  if (!serializeReportToJson(report, APP_REPORT, reportBuffer, REPORT_BUFFER_SIZE, written)) {
    return;
  }

  mqttClient.beginMessage(topics.reportTopic, false, 0);
  mqttClient.write((const uint8_t*)reportBuffer, written);
  mqttClient.endMessage();

  Serial.println("[REPORT] Published enrollment report.");
} 

// Used to build an event report initiated by the Opta
void publishEventStatusReport(CommandSource source) {
  ReportMessage report;
  if (!buildEventStatusReport(report, source)) {
    return;
  }

  static char reportBuffer[REPORT_BUFFER_SIZE];
  size_t written;
  if (!serializeReportToJson(report, APP_REPORT, reportBuffer, REPORT_BUFFER_SIZE, written)) {
    return;
  }

  mqttClient.beginMessage(topics.reportTopic, false, 0);
  mqttClient.write((const uint8_t*)reportBuffer, written);
  mqttClient.endMessage();

  Serial.print("[REPORT] Published event status report for source ");
  Serial.println(source);
}

// Parses a report request and initiates building and sending the reports
void parseReportRequest(const char* payloadStr) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payloadStr);
  if (err) {
    Serial.print("[REPORT] Request JSON error: ");
    Serial.println(err.c_str());
    return;
  }

  const char* requestType = doc["requestType"];
  if (requestType == nullptr || strcmp(requestType, "eventStatus") != 0) {
    Serial.println("[REPORT] Unrecognized or missing requestType, ignoring request.");
    return;
  }

  for (int i = 0; i < NUM_SOURCES; i++) {
    publishEventStatusReport((CommandSource)i);
  }
}

// Builds the enrollment and identity announcement report
bool buildEnrollmentReport(ReportMessage &report) {
  strncpy(report.reportName, "enrollmentReport", MAX_REPORTNAME_LEN - 1);
  report.reportName[MAX_REPORTNAME_LEN - 1] = '\0';

  strncpy(report.clientName, registration.venId, MAX_CLIENTNAME_LEN - 1);
  report.clientName[MAX_CLIENTNAME_LEN - 1] = '\0';

  report.hasEventID = false;
  report.eventID[0] = '\0';

  report.resourceCount = registration.resourceCount;
  if (report.resourceCount > MAX_RESOURCES) {
    Serial.println("[REPORT] WARNING: resourceCount exceeds MAX_RESOURCES, truncating.");
    report.resourceCount = MAX_RESOURCES;
  }

  for (uint8_t i = 0; i < report.resourceCount; i++) {
    strncpy(report.resourceEntries[i].resourceName,
            registration.resources[i].name,
            MAX_RESOURCEID_LEN- 1);
    report.resourceEntries[i].resourceName[MAX_RESOURCEID_LEN- 1] = '\0';
    report.resourceEntries[i].hasIntervalData = false;
  }

  if (report.resourceCount == 0) {
    Serial.println("[REPORT] WARNING: building enrollment report with zero configured resources.");
  }
  return true;
}

// Builds the event reports based on the given source's CommandSlot state
bool buildEventStatusReport(ReportMessage &report, CommandSource source) {
  const CommandSlot &slot = sources[source];

  if (!slot.active) {
    return false;
  }

  if (registration.resourceCount == 0) {
    Serial.println("[REPORT] Cannot build event status report: no resources configured.");
    return false;
  }

  strncpy(report.reportName, "eventStatusReport", MAX_REPORTNAME_LEN - 1);
  report.reportName[MAX_REPORTNAME_LEN - 1] = '\0';

  strncpy(report.clientName, registration.venId, MAX_CLIENTNAME_LEN - 1);
  report.clientName[MAX_CLIENTNAME_LEN - 1] = '\0';

  report.hasEventID = true;
  strncpy(report.eventID, slot.eventId, MAX_EVENTID_LEN - 1);
  report.eventID[MAX_EVENTID_LEN - 1] = '\0';

  // ToDo: every event status report currently
  // describes resources[0] specifically, since this device controls
  // exactly one relay. If EntWise ever controls multiple resources per
  // Opta, this needs to change to know which specific resource a given
  // command actually targets -- there's no such association tracked
  // anywhere yet (CommandSlot has no resource reference of its own).
  report.resourceCount = 1;
  strncpy(report.resourceEntries[0].resourceName,
          registration.resources[0].name,
          MAX_RESOURCEID_LEN- 1);
  report.resourceEntries[0].resourceName[MAX_RESOURCEID_LEN- 1] = '\0';
  report.resourceEntries[0].hasIntervalData = true;
  report.resourceEntries[0].payloadValue = slot.dispatchValue;

  return true;
}

// Serializes a ReportMessage into JSON using the given field-name map.
bool serializeReportToJson(const ReportMessage &report, const OpenADR3ReportFields &fields,
                            char* buffer, size_t bufSize, size_t &bytesWritten) {
  JsonDocument doc;

  doc[fields.reportName] = report.reportName;
  doc[fields.clientName] = report.clientName;

  if (report.hasEventID) {
    doc[fields.eventID] = report.eventID;
  }
  // eventID key omitted entirely when !hasEventID

  JsonArray resourcesArr = doc[fields.resources].to<JsonArray>();

  if (report.resourceCount > MAX_RESOURCES) {
    Serial.println("[REPORT] WARNING: resourceCount exceeds MAX_RESOURCES, truncating output.");
  }
  uint8_t count = (report.resourceCount > MAX_RESOURCES) ? MAX_RESOURCES : report.resourceCount;

  for (uint8_t i = 0; i < count; i++) {
    const ReportResourceEntry &entry = report.resourceEntries[i];
    JsonObject resourceObj = resourcesArr.add<JsonObject>();
    resourceObj[fields.resourceName] = entry.resourceName;

    if (entry.hasIntervalData) {
      JsonArray intervalsArr = resourceObj[fields.interval].to<JsonArray>();
      JsonObject intervalObj = intervalsArr.add<JsonObject>();
      intervalObj[fields.intervalId] = 0;   // ToDo: extend to multiple intervals when scheduler exists

      JsonArray payloadsArr = intervalObj[fields.payload].to<JsonArray>();
      JsonObject payloadObj = payloadsArr.add<JsonObject>();
      payloadObj[fields.payloadType] = PAYLOAD_TYPE_SIMPLE;

      JsonArray valuesArr = payloadObj[fields.payloadValue].to<JsonArray>();
      valuesArr.add(entry.payloadValue);
    }
    // No intervals/payloads key at all when !hasIntervalData -- matches
    // the enrollment report's identity-only resource entries.
  }

  bytesWritten = serializeJson(doc, buffer, bufSize);
  if (bytesWritten == 0 || bytesWritten >= bufSize) {
    Serial.println("[REPORT] Serialized report too large for buffer, aborting.");
    return false;
  }

  return true;
}



// -- Data Conversion Functions ---------------
