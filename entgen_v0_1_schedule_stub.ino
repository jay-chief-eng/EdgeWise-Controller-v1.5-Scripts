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

// NTP Configuration
const char*  NTP_SERVER       = "pool.ntp.org";
const int    NTP_PORT         = 123;
const int    NTP_PACKET_SIZE  = 48;
const uint32_t NTP_TIMEOUT_MS = 3000;   // wait per attempt for a reply
const int    NTP_MAX_ATTEMPTS = 5;

WiFiUDP ntpUdp;
byte    ntpPacketBuffer[NTP_PACKET_SIZE];

// Relay Configuration
bool dispatchGen = false;
bool prevDispatch = false;

// OpenADR JSON Configuration
// OpenADR Command names:
struct OpenAdr3EventFields {
  const char* eventName;
  const char* programID;
  const char* period;
  const char* periodStart;
  const char* periodDur;
  const char* interval;
  const char* intervalId;
  const char* payload;
  const char* payloadType;
  const char* payloadValue;
};

const OpenAdr3EventFields APP_COMMAND = {
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

// Command Schedule Configuration
#define MAX_INTERVALS 96   // 24h at 15-min minimum resolution
#define MIN_DURATION_SECONDS  (15 * 60) // Limits duration to no less than 15 minutes
#define MAX_DURATION_SECONDS  (24 * 3600) // Limits duration to no more than 24 hours
#define MAX_START_HORIZON_SECONDS (24 * 3600) // Limits intervals to start no more than 24 hours from now
#define MAX_EVENTNAME_LEN 32

struct DispatchInterval {
  char    eventName[MAX_EVENTNAME_LEN];     // Name of event that scheduled this interval
  time_t  startTime;                        // epoch seconds, converted from ISO-8601
  time_t  endTime;                          // startTime + duration
  uint8_t dispatchValue;                    // 0 = grid, 1 = generator
  uint8_t priority;                         // 1-99, default 1 for now
};

DispatchInterval schedule[MAX_INTERVALS];
int scheduleHead = 0;  // physical index of the current logical front (interval 0)

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

  // Initialize relay output
  pinMode(D0, OUTPUT);
  pinMode(LED_D0, OUTPUT);
  digitalWrite(D0, LOW);
  digitalWrite(LED_D0, LOW);

  // Initialize the schedule
  initializeSchedule()
}


// --Main Program Loop-------------------------
// Code here runs continuously

void loop() {
  // put your main code here, to run repeatedly:

  // Update the schedule
  maintainScheduleWindow()

  // Evaluate trigger commands and dispatch if called for
  // Check the schedule
  evaluateScheduleDispatch()

  // Trigger the relay if called for
  updateRelayOutput()
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

// -- Time synchronization --------------------

// Builds and sends a minimal NTP client request packet.
void sendNtpPacket() {
  memset(ntpPacketBuffer, 0, NTP_PACKET_SIZE);
  ntpPacketBuffer[0] = 0b11100011;   // LI=3 (unknown), VN=4, Mode=3 (client)
  ntpPacketBuffer[1] = 0;            // Stratum, unspecified
  ntpPacketBuffer[2] = 6;            // Polling interval
  ntpPacketBuffer[3] = 0xEC;         // Peer clock precision
  // Bytes 12-23 (reference/origin/receive timestamps) left as zero

  ntpUdp.beginPacket(NTP_SERVER, NTP_PORT);
  ntpUdp.write(ntpPacketBuffer, NTP_PACKET_SIZE);
  ntpUdp.endPacket();
}

// Attempts to sync system time via NTP. Returns true on success.
// Requires WiFi to already be connected -- does not attempt that itself.
bool syncTimeViaNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[NTP] WiFi not connected, cannot sync time");
    return false;
  }

  ntpUdp.begin(2390);   // arbitrary local port for the UDP socket

  for (int attempt = 1; attempt <= NTP_MAX_ATTEMPTS; attempt++) {
    Serial.print("[NTP] Requesting time, attempt ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.println(NTP_MAX_ATTEMPTS);

    sendNtpPacket();

    unsigned long waitStart = millis();
    while (millis() - waitStart < NTP_TIMEOUT_MS) {
      if (ntpUdp.parsePacket() >= NTP_PACKET_SIZE) {
        ntpUdp.read(ntpPacketBuffer, NTP_PACKET_SIZE);

        // Seconds since 1900 live in bytes 40-43, big-endian.
        uint32_t secsSince1900 =
          ((uint32_t)ntpPacketBuffer[40] << 24) |
          ((uint32_t)ntpPacketBuffer[41] << 16) |
          ((uint32_t)ntpPacketBuffer[42] << 8)  |
          ((uint32_t)ntpPacketBuffer[43]);

        if (secsSince1900 == 0) {
          Serial.println("[NTP] Received empty/invalid response, retrying");
          break;  // out of the inner wait loop, on to next attempt
        }

        // NTP epoch (1900) to Unix epoch (1970) offset
        const uint32_t SEVENTY_YEARS = 2208988800UL;
        time_t epochTime = (time_t)(secsSince1900 - SEVENTY_YEARS);

        set_time(epochTime);  // Mbed OS RTC set -- makes time(nullptr) return this going forward

        Serial.print("[NTP] Synced. Epoch time: ");
        Serial.println((unsigned long)epochTime);
        return true;
      }
    }
    Serial.println("[NTP] No response within timeout");
  }

  Serial.println("[NTP] Failed to sync after all attempts");
  return false;
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

// -- App Control Functions -------------------

void parseOpenADR3Dispatch(const char* payload, const OpenAdrEventFields& fields, const char* regProgram) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("[PARSE] JSON error: ");
    Serial.println(err.c_str());
    return;
  }
  if (doc[fields.programID].isNull() || doc[fields.interval].isNull()) {
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

  const char* eventName = doc[fields.eventName];

  JsonArray intervals = doc[fields.interval].as<JsonArray>();
  for (JsonObject interval : intervals) {
    if (interval[fields.intervalID].isNull() || interval[fields.payload].isNull()) {
      Serial.println("[PARSE] Interval missing id or payloads, skipping this interval.");
      continue;
    }

    const char* startStr;
    const char* durStr;
    bool hasOwnPeriod = interval[fields.period].is<JsonObject>();
    if (hasOwnPeriod) {
      startStr = interval[fields.period][fields.periodStart];
      durStr   = interval[fields.period][fields.periodDur];
    } else if (doc[fields.period].is<JsonObject>()) {
      startStr = doc[fields.period][fields.periodStart];
      durStr   = doc[fields.period][fields.periodDur];
    } else {
      Serial.println("[PARSE] No intervalPeriod at event or interval level, skipping interval.");
      continue;
    }

    if (startStr == nullptr) {
      Serial.println("[PARSE] Missing start time, skipping this interval");
      continue;
    }

    // OpenADR 3.0 "do it now" sentinel (§7.3). Treated here as a signal to
    // bypass the schedule entirely, not just "start = now" within it -- see
    // caveat above this function about that being a design choice on our
    // part, not a spec requirement.
    bool isImmediate = (strcmp(startStr, "0001-01-01T00:00:00") == 0) ||
                        (strcmp(startStr, "0001-01-01") == 0);

    JsonArray payloads = interval[fields.payload].as<JsonArray>();
    if (payloads.size() == 0) {
      Serial.println("[PARSE] No payloads in interval, skipping this interval");
      continue;
    }
    JsonVariant rawValue = payloads[0][fields.payloadValue][0];
    if (rawValue.isNull()) {
      Serial.println("[PARSE] No dispatch value in payload, skipping");
      continue;
    }
    uint8_t dispatchValue = rawValue.as<int>();

    if (isImmediate) {
      handleImmediateAction(dispatchValue, eventName);
      continue;  // does not touch the schedule at all
    }

    time_t startEpoch;
    if (!parseISO8601ToEpoch(startStr, startEpoch)) {
      Serial.println("[PARSE] Unparseable start time, skipping this interval");
      continue;
    }
    if (startEpoch - time(nullptr) > MAX_START_HORIZON_SECONDS) {
      Serial.println("[PARSE] Start time more than 24h out, rejecting interval");
      continue;
    }
    time_t endEpoch;
    if (!parseISO8601DurationToEpoch(startEpoch, durStr, endEpoch)) {
      Serial.println("[PARSE] Unparseable duration, skipping this interval");
      continue;
    }

    uint8_t priority = 1;  // TODO: no priority field parsed yet -- hardcoded until
                            // app vs. coop event priority conventions are defined
    upsertScheduleEvent(startEpoch, endEpoch, dispatchValue, priority, eventName);
  }
}

// -- App Telemetry Functions -----------------

// -- Data Conversion Functions ---------------
// Time conversion helper
bool parseISO8601ToEpoch(const char* isoStr, time_t &outEpoch) {
  if (isoStr == nulptr) return fals;
  struct tm tmStruct = {0};
  int matched = sscanf(isoStr, "%d-%d-%dT%d:%d:%d",
                        &tmStruct.tm_year, &tmStruct.tm_mon, %tmStruct.tm_mday,
                        &tmStruct.tm_hour, &tmStruct.tm_min, &tmStruct.tm_sec);
  if (matched < 6) return false;
  tmStruct.tm_year  -= 1900;
  tmStruct.tm_mon   -= 1;
  outEpoch = timegm(&tmStruct); // UTC
  return true
}

// Duration conversion helper
bool parseISO8601DurationToEpoch(time_t startEpoch, const char* durStr, time_t &outEpoch) {
  if (durStr == nullptr || durStr[0] != 'P') {
    return false;
  }

  const char* p = durStr + 1;  // skip leading 'P'
  bool inTimePart = false;     // true once we've passed the 'T' separator
  long totalSeconds = 0;
  long clampedTotalSeconds = 0;
  bool matchedAnyUnit = false;

  while (*p != '\0') {
    if (*p == 'T') {
      inTimePart = true;
      p++;
      continue;
    }

    char* numEnd;
    long value = strtol(p, &numEnd, 10);
    if (numEnd == p || *numEnd == '\0') {
      // no digits consumed, or a number with no unit letter after it
      Serial.println("[PARSE] Malformed duration string");
      return false;
    }

    char unit = *numEnd;
    p = numEnd + 1;

    switch (unit) {
      case 'H':
        if (!inTimePart) { Serial.println("[PARSE] 'H' outside time part"); return false; }
        totalSeconds += value * 3600L;
        matchedAnyUnit = true;
        break;
      case 'M':
        if (!inTimePart) {
          // 'M' before 'T' means months in ISO-8601 -- ambiguous length
          // (28-31 days), not convertible to a fixed second count.
          Serial.println("[PARSE] Month durations not supported");
          return false;
        }
        totalSeconds += value * 60L;
        matchedAnyUnit = true;
        break;
      case 'S':
        if (!inTimePart) { Serial.println("[PARSE] 'S' outside time part"); return false; }
        totalSeconds += value;
        matchedAnyUnit = true;
        break;
      case 'D':
        totalSeconds += value * 86400L;
        matchedAnyUnit = true;
        break;
      default:
        // Covers 'Y' (years -- ambiguous length, same issue as months)
        // and anything else unrecognized.
        Serial.print("[PARSE] Unsupported duration unit: ");
        Serial.println(unit);
        return false;
    }
  }

  if (!matchedAnyUnit) {
    return false;  // e.g. bare "PT" with nothing after it
  }

  // Clamp durations between 15 minutes and 24 hours
  if (totalSeconds > 0 && totalSeconds < MIN_DURATION_SECONDS) {
    clampedTotalSeconds = MIN_DURATION_SECONDS;
    Serial.println("[PARSE] Duration below 15-minute minimum, rounded up");
  } else if (totalSeconds > MAX_DURATION_SECONDS) {
    clampedTotalSeconds = MAX_DURATION_SECONDS;
    Serial.println("[PARSE] Duration exceeds 24-hour maximum, truncated");
  } else {
    clampedTotalSeconds = totalSeconds;
  }

  outEpoch = startEpoch + (time_t)clampedTotalSeconds;
  return true;
}

// Schedule maintenance functions

// Schedule head finder.
// The schedule head can be at a non-zero location in the array since
// the schedule uses a ring bus. 
DispatchInterval& getScheduleSlot(int logicalIndex) {
  return schedule[(scheduleHead + logicalIndex) % MAX_INTERVALS];
}

// Schedule initialization helper.
// Populates the schedule with 96 contiguous 15-minute default intervals,
// starting from the current time. 
void initializeSchedule() {
  time_t nowTime = time(nullptr);
  scheduleHead = 0;  // fresh window: logical position 0 == physical index 0

  for (int i = 0; i < MAX_INTERVALS; i++) {
    DispatchInterval &slot = getScheduleSlot(i);
    slot.startTime     = nowTime + (i * MIN_DURATION_SECONDS);
    slot.endTime       = slot.startTime + MIN_DURATION_SECONDS;
    slot.dispatchValue = 0;    // grid
    slot.priority      = 99;   // default -- lowest priority, always overridable
    strncpy(slot.eventName, "default", MAX_EVENTNAME_LEN - 1);
    slot.eventName[MAX_EVENTNAME_LEN - 1] = '\0';
  }

  Serial.print("[SCHED] Initialized ");
  Serial.print(MAX_INTERVALS);
  Serial.println(" default intervals");
}

// Schedule maintainer
// Updates the schedule window as time passes. Drops any expired 
// interval(s) from the front of the window and appends an equal 
// number of fresh default intervals to the tail -- without 
// shifting any existing data.
void maintainScheduleWindow() {
  time_t nowTime = time(nullptr);

  while (schedule[scheduleHead].endTime <= nowTime) {
    int tailIndex = (scheduleHead + MAX_INTERVALS - 1) % MAX_INTERVALS;
    time_t newStart = schedule[tailIndex].endTime;

    DispatchInterval &slot = schedule[scheduleHead];
    slot.startTime     = newStart;
    slot.endTime        = newStart + MIN_DURATION_SECONDS;
    slot.dispatchValue = 0;     // grid
    slot.priority      = 99;    // default
    strncpy(slot.eventName, "default", MAX_EVENTNAME_LEN - 1);
    slot.eventName[MAX_EVENTNAME_LEN - 1] = '\0';

    scheduleHead = (scheduleHead + 1) % MAX_INTERVALS;
  }
}

// Event scheduler
// Applies a parsed event to the schedule.
// Any 15-minute slot that overlaps [newStart, newEnd) is replaced with 
// this event's value/priority/name, but only if this event's priority is 
// equal to or numerically lower than (i.e., equal or higher precedence than) 
// whatever currently occupies that slot. 
// Slot boundaries themselves are never modified -- only content.
bool upsertScheduleEvent(time_t newStart, time_t newEnd, uint8_t dispatchValue,
                          uint8_t priority, const char* eventName) {
  if (newStart >= newEnd) {
    Serial.println("[SCHED] Rejected: zero/negative-length interval");
    return false;
  }

  bool anySlotUpdated = false;
  bool anySlotBlockedByPriority = false;

  for (int i = 0; i < MAX_INTERVALS; i++) {
    DispatchInterval &slot = getScheduleSlot(i);

    bool overlaps = (slot.startTime < newEnd) && (slot.endTime > newStart);
    if (!overlaps) continue;

    if (priority <= slot.priority) {
      slot.dispatchValue = dispatchValue;
      slot.priority      = priority;
      strncpy(slot.eventName, eventName, MAX_EVENTNAME_LEN - 1);
      slot.eventName[MAX_EVENTNAME_LEN - 1] = '\0';
      anySlotUpdated = true;
    } else {
      anySlotBlockedByPriority = true;
    }
  }

  if (!anySlotUpdated && !anySlotBlockedByPriority) {
    Serial.print("[SCHED] Event '");
    Serial.print(eventName);
    Serial.println("' does not overlap the current 24h window, no slots updated");
    reportSchedulingWarning(eventName, "outside_window");
    return false;
  }

  if (anySlotBlockedByPriority) {
    Serial.print("[SCHED] Event '");
    Serial.print(eventName);
    Serial.println("' partially or fully blocked by a higher-priority existing event");
    reportSchedulingWarning(eventName, "priority_blocked");
  }

  return anySlotUpdated;
}