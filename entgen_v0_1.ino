// ============================================================
// EntGen Controller v0.1
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
#include <ArduinoRS485.h>
#include <ArduinoModbus.h>
#include <SPI.h>
#include <Ethernet.h>
#include <ArduinoMqttClient.h>
#include <ArduinoJson.h>
#include <mbed.h>

// -- Configuration Section --------------------

// Network configuration
// Wifi Credentials, MAC address, and IP configuration
const char* WIFI_SSID = "Trumbull";
const char* WIFI_PASSWORD = "xcelsior97";
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress localIP_wifi  (10, 0, 0, 101); // Opta IP for WiFi
IPAddress localIP_eth   (10, 0, 0, 102); // Opta IP for Ethernet port
IPAddress gateway       (10, 0, 0, 1);
IPAddress subnet        (255, 255, 255, 0);
IPAddress dns           (8, 8, 8, 8);
IPAddress server        (10, 0, 0, 103);        // Meter IP

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
const char* BROKER_IP   = "10.0.0.100";
const int   BROKER_PORT = 1883;
MqttClient  mqttClient(wifiClient);
// MQTT Topics
const char* TOPIC_STATUS        = "opta/status";
const char* TOPIC_APP_DISPATCH  = "opta/commands/app/dispatch";

// Relay Configuration
bool dispatchGen = false;
bool prevDispatch = false;

// OpenADR Asset Configuration
const char* regProgramApp = "44"; // the asset's registered programID

// OpenADR JSON Templates Configuration
// OpenADR Command names:
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

  WiFi.config(localIP_wifi, dns, gateway, subnet);   
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
    mqttClient.subscribe(TOPIC_APP_DISPATCH, 1);
    mqttClient.onMessage(onMqttMessage);
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
      mqttClient.subscribe(TOPIC_APP_DISPATCH, 1);
      mqttClient.onMessage(onMqttMessage);
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

  resolveDispatchAuthority();
  updateRelayOutput();
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
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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
  Serial.println(WIFI_SSID);

  if (WiFi.localIP() == localIP_wifi) {
    Serial.print("Static IP configured successfully. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.print("Warning: IP mismatch. Got: ");
    Serial.println(WiFi.localIP());
    Serial.print("Expected: ");
    Serial.println(localIP_wifi);
    Serial.println("Continuing with assigned IP — update localIP in sketch if needed.");
  }
  return true;
}

// Ethernet connection
// Makes 10 attempts to connect to the Ethernet
// Provides troubleshooting for the connection if it fails
bool connectEthernet() {
  Serial.print("Connecting to Ethernet");
  Ethernet.begin(mac, localIP_eth, dns, gateway, subnet);

  int attempts = 0;
  const int MAX_ATTEMPTS = 10;

  switch (Ethernet.hardwareStatus()) {
    case EthernetNoHardware:
      Serial.println("  EthernetNoHardware - controller not connected, check board/wiring");
      return false;

    case EthernetMbed: {
      bool linkUp = false;
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
      linkUp = true;
      break;
    }

    default:
      Serial.print("  Unrecognized hardwareStatus() code: ");
      Serial.println(Ethernet.hardwareStatus());
      return false;
  }

  Serial.println(" Link up.");
  delay(500);

  if (Ethernet.localIP() == localIP_eth) {
    Serial.print("Static IP configured successfully. IP: ");
    Serial.println(Ethernet.localIP());
  } else {
    Serial.print("Warning: IP mismatch. Got: ");
    Serial.println(Ethernet.localIP());
    Serial.print("Expected: ");
    Serial.println(localIP_eth);
    Serial.println("Continuing with assigned IP — update localIP in sketch if needed.");
  }
  return true;
}

// -- Modbus Functions ------------------------

// TCP Setup
void setupTCP() {
  if (!modbusTCPClient.begin(server, MODBUS_TCP_PORT)) {
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
  mqttClient.beginWill(TOPIC_STATUS, true, 1);
  mqttClient.print("offline");
  mqttClient.endWill();

  Serial.print("Connecting to MQTT broker");
  int attempts = 0;
  const int MAX_ATTEMPTS = 10;

  while (!mqttClient.connect(BROKER_IP, BROKER_PORT)) {
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

  mqttClient.beginMessage(TOPIC_STATUS, true);
  mqttClient.print("online");
  mqttClient.endMessage();
  Serial.println("Announced presence by setting presence to online");
  return true;
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

  if (topic == TOPIC_APP_DISPATCH) {
    parseOpenADR3Dispatch(payloadStr.c_str(), APP_COMMAND, regProgramApp, SOURCE_APP);
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

// -- App Telemetry Functions -----------------

// -- Data Conversion Functions ---------------
// ============================================================
// EntGen Controller v0.1
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
#include <ArduinoRS485.h>
#include <ArduinoModbus.h>
#include <Ethernet.h>
#include <ArduinoMqttClient.h>
#include <Arduinojson.h>
#include <mbed.h>

// -- Configuration Section --------------------

// Network configuration
// Wifi Credentials, MAC address, and IP configuration
const char* WIFI_SSID = "Trumbull";
const char* WIFI_PASSWORD = "xcelsior97";
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress localIP_wifi  (10, 0, 0, 101); // Opta IP for WiFi
IPAddress localIP_eth   (10, 0, 0, 102); // Opta IP for Ethernet port
IPAddress gateway       (10, 0, 0, 1);
IPAddress subnet        (255, 255, 255, 0);
IPAddress dns           (8, 8, 8, 8);
IPAddress server        (10, 0, 0, 103);        // Meter IP

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
const char* BROKER_IP   = "10.0.0.100";
const int   BROKER_PORT = 1883;
MqttClient  mqttClient(wifiClient);
// MQTT Topics
const char* TOPIC_STATUS        = "opta/status";
const char* TOPIC_APP_DISPATCH  = "opta/commands/app/dispatch";

WiFiUDP ntpUdp;
byte    ntpPacketBuffer[NTP_PACKET_SIZE];

// Relay Configuration
bool dispatchGen = false;
bool prevDispatch = false;

// OpenADR Asset Configuration
const char* regProgramApp = "44"; // the asset's registered programID

// OpenADR JSON Templates Configuration
// OpenADR Command names:
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

// Sentinel values per OpenADR 3.0 §7.3 / §7.9
// Used to determine if a command is for an instant command or to clear a command
static const char* SENTINEL_START         = "0001-01-01T00:00:00";
static const char* SENTINEL_START_ALT     = "0001-01-01";
static const char* SENTINEL_DURATION_INF  = "P9999Y";
static const char* SENTINEL_DURATION_ZERO = "PT0S";

// Parsed command template

enum CommandClassification { CMD_ASSERT, CMD_RELEASE, CMD_UNSUPPORTED_SCHEDULE };

struct ParsedCommand {
  char                  eventId[MAX_EVENTID_LEN];
  char                  eventName[MAX_EVENTNAME_LEN];
  uint8_t               dispatchValue;   // only meaningful when classification == CMD_ASSERT
  CommandClassification classification;
};

// Command Arbitration Configuration
// Lower number = higher precedence.
#define PRIORITY_APP      1
#define PRIORITY_COOP     50
#define PRIORITY_DEFAULT  99

#define MAX_EVENTID_LEN   32
#define MAX_EVENTNAME_LEN 32

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

// -- Setup Section ----------------------------
// Code here runs once

void setup() {
  // Set up serial interface to output status information
  Serial.begin(115200);
  while (!Serial);

  // Indicate that program successfully launched
  Serial.println("Program has begun.");

  // Start WiFi
  WiFi.config(localIP_wifi, gateway, subnet, dns);
  connectWiFi();

  // Start Ethernet
  connectEthernet();

  // Synchronize time
  syncTimeViaNTP()

  // Start Modbus
  #ifdef USE_MODBUS_TCP
    setupTCP();
  #endif

  #ifdef USE_MODBUS_RTU
    setupRTU();
  #endif

  // Start MQTT
  connectMQTT();
  mqttClient.subscribe(TOPIC_APP_DISPATCH, 1);
  mqttClient.onMessage(onMqttMessage);
  // TODO: subscribe() + onMessage() above must be repeated inside the
  // reconnection handler once built -- MQTT subscriptions do not survive
  // a broker disconnect/reconnect automatically.

  // Initialize control source
  initializeDefaultSource();

  // Initialize relay output
  pinMode(D0, OUTPUT);
  pinMode(LED_D0, OUTPUT);
  digitalWrite(D0, LOW);
  digitalWrite(LED_D0, LOW);

}


// --Main Program Loop-------------------------
// Code here runs continuously

void loop() {
  // put your main code here, to run repeatedly:

  // Check for mqtt messages
  mqttClient.poll();

  // Trigger the relay if called for
  updateRelayOutput();
}

// -- WiFi and Ethernet Functions -------------

// WiFi connection
// Makes 10 attempts to connect to the WiFi.
// Provides troubleshooting for the connection if it fails.
void connectWiFi() {
  WiFi.disconnect();
  WiFi.end();
  delay(1000);
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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
      Serial.println("Halting. Reset the Opta to retry.");
      while (1);
    }
  }

  Serial.println();
  Serial.print("WiFi connected to: ");
  Serial.println(WIFI_SSID);

  // Confirm the IP was assigned correctly
  if (WiFi.localIP() == localIP_wifi) {
    Serial.print("Static IP configured successfully. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.print("Warning: IP mismatch. Got: ");
    Serial.println(WiFi.localIP());
    Serial.print("Expected: ");
    Serial.println(localIP_wifi);
    Serial.println("Continuing with assigned IP — update localIP in sketch if needed.");
  }
}

// Ethernet connection
// Makes 10 attempts to connect to the Ethernet
// Provides troubleshooting for the connection if it fails
void connectEthernet() {
  Serial.print("Connecting to Ethernet");
  Ethernet.begin(mac, localIP_eth, gateway, subnet);

  int attempts = 0;
  const int MAX_ATTEMPTS = 10;

  // Validate that Ethernet hardware is present
  switch (Ethernet.hardwareStatus()) {
    case EthernetNoHardware:
      Serial.println("  EthernetNoHardware - controller not connected, check board/wiring");
      Serial.println("Halting. Reset the Opta to retry.");
      while (1);
      break;

    case EthernetW5500:
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
          Serial.println("Halting. Reset the Opta to retry.");
          while (1);
          break;
        }
      }
      break;

    default:
      Serial.print("  Unrecognized hardwareStatus() code: ");
      Serial.println(Ethernet.hardwareStatus());
      Serial.println("Halting. Reset the Opta to retry.");
      while (1);
      break;
  }
  Serial.println(" Link up.");
  delay(500);  // brief settle time after link comes up

  // Confirm the IP was assigned correctly
  if (Ethernet.localIP() == localIP_eth) {
    Serial.print("Static IP configured successfully. IP: ");
    Serial.println(Ethernet.localIP());
  } else {
    Serial.print("Warning: IP mismatch. Got: ");
    Serial.println(Ethernet.localIP());
    Serial.print("Expected: ");
    Serial.println(localIP_eth);
    Serial.println("Continuing with assigned IP — update localIP in sketch if needed.");
  }
}

// -- Modbus Functions ------------------------

// TCP Setup
void setupTCP() {
  if (!modbusTCPClient.begin(server, MODBUS_TCP_PORT)) {
    Serial.print("Failed to connect to Modbus TCP server. Error: ");
    Serial.println(modbusTCPClient.lastError());
    Serial.println("Halting.");
    while (1);
  }
  Serial.println("Modbus TCP connected.");
}

// RTU Setup
void setupRTU() {
  // Default device address 1, 9600 baud, 8N1
  if (!ModbusRTUClient.begin(9600)) {
    Serial.print("Failed to start Modbus RTU. Error: ");
    Serial.println(ModbusRTUClient.lastError());
    Serial.println("Halting.");
    while (1);
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

// Function to check if the modbus client is still connected.
// The function will attempt to reconnect if connection has failed. 
bool ensureConnected() {
  if (modbusTCPClient.connected()) return true;

  Serial.println("Modbus TCP disconnected. Reconnecting...");
  if (!modbusTCPClient.begin(server, MODBUS_TCP_PORT)) {
    Serial.print("Reconnect failed. Error code: ");
    Serial.println(modbusTCPClient.lastError());
    return false;
  }
  Serial.println("Reconnected successfully.");
  return true;
}

// -- MQTT Functions --------------------------

void connectMQTT() {
  // Last Will: broker publishes "offline" to opta/status if we drop
  mqttClient.beginWill(TOPIC_STATUS, true, 1);
  mqttClient.print("offline");
  mqttClient.endWill();

  Serial.print("Connecting to MQTT broker");
  int attempts = 0;
  const int MAX_ATTEMPTS = 10;

  while (!mqttClient.connect(BROKER_IP, BROKER_PORT)) {
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
      Serial.println("Max attempts reached. Halting.");
      while (1);
    }
    delay(500);
  }
  Serial.println();
  Serial.println("MQTT connected.");

  // Announce presence
  mqttClient.beginMessage(TOPIC_STATUS, true);    // retain = true
  mqttClient.print("online");
  mqttClient.endMessage();
  Serial.println("Announced presence by setting presence to online");
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

  if (topic == TOPIC_APP_DISPATCH) {
    parseOpenADR3Dispatch(payloadStr.c_str(), APP_COMMAND, regProgramApp, SOURCE_APP);
  } else {
    Serial.print("[MQTT] No handler registered for topic: ");
    Serial.println(topic);
  }
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

  sources[SOURCE_APP]  = { "", "", 0, PRIORITY_APP,  false, 0 };
  sources[SOURCE_COOP] = { "", "", 0, PRIORITY_COOP, false, 0 };

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

  resolveDispatchAuthority();
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
    updateRelayOutput();
    return;
  }

  dispatchGen = (sources[winner].dispatchValue == 1);

  Serial.print("[COMMAND] Arbitration winner: source "); Serial.print(winner);
  Serial.print(" (priority "); Serial.print(sources[winner].priority);
  Serial.print(") -> dispatchGen="); Serial.println(dispatchGen ? "true" : "false");

  updateRelayOutput();
}

// -- App Telemetry Functions -----------------

// -- Data Conversion Functions ---------------
