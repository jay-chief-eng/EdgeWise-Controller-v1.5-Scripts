// ============================================================
//  opta_ven.ino - Arduino Opta OpenADR 2.0b VEN
//  Test protocol:
//    1. Mount flash filesystem, load XML template on startup
//    2. Connect to WiFi (172.16.0.2 static)
//    3. Register with VTN (172.16.0.1:8080)
//    4. Poll VTN every 1 second
//    5. On SIMPLE signal 1 -> shed 3 kW, report back
//    6. On SIMPLE signal 2 -> shed 10 kW, report back
//
//  All XML is built in RAM from the template loaded at boot.
//  No XML file writes occur after startup
// ============================================================

#include <WiFi.h>
#include <ArduinoHttpClient.h>
#include <BlockDevice.h>
#include <LittleFileSystem.h>
#include <MBRBlockDevice.h>

using namespace mbed;

// -- Configuration Section --------------------

// MAC address
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

// Network config
const char* WIFI_SSID = "Trumbull";
const char* WIFI_PASSWORD = "xcelsior97";
IPAddress   localIP     (10, 0, 0, 101);
IPAddress   gateway     (10, 0, 0, 1);
IPAddress   subnet      (255, 255, 255, 0);
IPAddress   dns         (8, 8, 8, 8);
const char* VTN_HOST    = "10.0.0.100";
const int   VTN_PORT    = 8080;
const char* VTN_PATH_REGISTER = "/OpenADR2/Simple/2.0b/EiRegisterParty";
const char* VTN_PATH_REGISTER_REPORT = "/OpenADR2/Simple/2.0b/EiReport";
const char* VTN_PATH_POLL     = "/OpenADR2/Simple/2.0b/OadrPoll";
const char* VTN_PATH_EVENT    = "/OpenADR2/Simple/2.0b/EiEvent";
const char* VTN_PATH_REPORT   = "/OpenADR2/Simple/2.0b/EiReport";

// Device identity
const char* VEN_ID      = "ven1234";
const char* VTN_ID      = "test-vtn";

// Filesystem
BlockDevice*      bd = BlockDevice::get_default_instance();
MBRBlockDevice    sys_bd(bd, 1);    // partition 1 - system/Wifi
MBRBlockDevice    usr_bd(bd, 2);    // partition 2 - user data
LittleFileSystem  fs("fs");
const char*       TEMPLATE_PATH = "/fs/openadr_template.xml";

// Template sections - loaded once at boot
// Each section is one XML block from the template file.
// Stored as String so we can use replace() in RAM.
String templPartyRegistration;  // oadrCreatePartyRegistration
String templRegister;         // oadrRegisterReport
String templPoll;             // oadrPoll
String templCreatedEvent;     // oadrCreatedEvent
String templUpdateReport;     // oadrUpdateReport

// Runtime State
bool    registered      = false;
int     requestSeq      = 0;
int     reportSeq       = 0;
String  currentEventID  = "";

unsigned long lastPoll = 0;
const unsigned long POLL_INTERVAL = 1000;     // 1 second

// HTTP client
WiFiClient  wifi;
HttpClient  http(wifi, VTN_HOST, VTN_PORT);

// Retry configuration
const int MAX_POLL_RETRIES = 3;
int pollFailCount = 0;
const int MAX_POST_RETRIES = 3;

// -- Setup Section ----------------------------
// Code here runs once

void setup() {
  Serial.begin(9600);
  while (!Serial);
  Serial.println("Opta OpenADR VEN starting...");

  // File uploads
  mountFilesystem();
  loadTemplates();

  // WiFi configuration and startun
  WiFi.config(localIP, gateway, subnet, dns);
  connectWiFi();

  // Start the VEN
  registerVEN();
}

// --Main Program Loop-------------------------
// Code here runs continuously

void loop() {
  if (millis() - lastPoll >= POLL_INTERVAL) {
    lastPoll = millis();
    pollVTN();
  }
}

// -- Function call definitions ----------------

// Filesystem
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

// Load and print template into sections
// Reads the file once and extracts each named XML block into
// its own String. All subsequent XML building uses these
// in-RAM strings - no further file reads.
void loadTemplates() {
  Serial.print("Loading template from ");
  Serial.println(TEMPLATE_PATH);

  FILE* f = fopen(TEMPLATE_PATH, "r");
  if (!f) {
    Serial.println("ERROR: template file not found.");
    Serial.println("Run the upload utility first.");
    while(1);
  }

  String raw = "";
  char buf[128];
  while (fgets(buf, sizeof(buf), f)) {
    raw += buf;
  }
  fclose(f);

  Serial.print("Template loaded. Size: ");
  Serial.print(raw.length());
  Serial.println(" bytes");

  // Extract each section between its opening and closing tags
  templPartyRegistration = extractSection(raw, 
                           "oadr:oadrCreatePartyRegistration");
  templRegister          = extractSection(raw, 
                           "oadr:oadrRegisterReport");
  templPoll              = extractSection(raw, "oadr:oadrPoll");
  templCreatedEvent      = extractSection(raw, "oadr:oadrCreatedEvent");
  templUpdateReport      = extractSection(raw, "oadr:oadrUpdateReport");

  Serial.println("Templates parsed into RAM sections:");
  Serial.println("  oadrCreatePartyRegistration : " +
                 String(templPartyRegistration.length()) + " bytes");
  Serial.println("  oadrRegisterReport          : " +
                 String(templRegister.length())          + " bytes");
  Serial.println("  oadrPoll                    : " +
                 String(templPoll.length())              + " bytes");
  Serial.println("  oadrCreatedEvent            : " +
                 String(templCreatedEvent.length())      + " bytes");
  Serial.println("  oadrUpdateReport            : " +
                 String(templUpdateReport.length())      + " bytes");

  // Halt if any critical template is missing
  if (templPartyRegistration.length() == 0 ||
      templPoll.length()              == 0 ||
      templCreatedEvent.length()      == 0 ||
      templUpdateReport.length()      == 0) {
    Serial.println("ERROR: one or more critical templates missing.");
    Serial.println("Check template file and re-upload.");
    while(1);
  }

  Serial.println("All templates loaded successfully.");
}

// Extracts <tagName>...</tagName> block from larger string
// This is key to breaking apart a larger XML file into smaller parts
String extractSection(const String& src, const String& tag) {
  String openStart = "<" + tag;   // matches tag with any attributes
  String close     = "</" + tag + ">";
  
  int start = src.indexOf(openStart);
  if (start < 0) {
    Serial.println("Warning: section not found: " + tag);
    return "";
  }
  
  // Find the end of the opening tag (could have attributes)
  int tagEnd = src.indexOf(">", start);
  if (tagEnd < 0) {
    Serial.println("Warning: malformed tag: " + tag);
    return "";
  }

  int end = src.indexOf(close, tagEnd);
  if (end < 0) {
    Serial.println("Warning: closing tag not found: " + tag);
    return "";
  }

  return src.substring(start, end + close.length());
}

// Token replacement - builds a message from a template
// Takes a template section, replaces all {{TOKEN}} placeholders,
// raps in the standard oadrPayload envelope, and returns as String.
String buildMessage(const String& tmpl,
                    const String & requestID) {
  String msg = tmpl;
  msg.replace("{{VEN_ID}}",     VEN_ID);
  msg.replace("{{VTN_ID}}",     VTN_ID);
  msg.replace("{{REQUEST_ID}}", requestID);
  return wrapEnvelope(msg);
}

// Wrap an XML body in the standard OpenADR payload envelope
String wrapEnvelope(const String& body) {
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<oadr:oadrPayload "
      "xmlns:oadr=\"http://openadr.org/oadr-2.0b/2012/07\">"
      "<oadr:oadrSignedObject "
        "xmlns:oadr=\"http://openadr.org/oadr-2.0b/2012/07\" "
        "oadr:Id=\"oadrSignedObject\">" +
        body +
      "</oadr:oadrSignedObject>"
    "</oadr:oadrPayload>";
}

// WiFi
void connectWiFi() {
  // Stop any previous connection and wait for WiFi module to initialize
  WiFi.disconnect();
  delay(1000);  // give module time to fully initialize

  // Wait for module to be ready before scanning
  Serial.print("Waiting for WiFi module");
  int moduleWait = 0;
  while (WiFi.status() == WL_NO_SHIELD && moduleWait < 10) {
    delay(500);
    Serial.print(".");
    moduleWait++;
  }
  Serial.println();

  if (WiFi.status() == WL_NO_SHIELD) {
    Serial.println("WiFi module not found. Halting.");
    while(1);
  }

  // Start new connection
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
  if (WiFi.localIP() == localIP) {
    Serial.print("Static IP configured successfully. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.print("Warning: IP mismatch. Got: ");
    Serial.println(WiFi.localIP());
    Serial.print("Expected: ");
    Serial.println(localIP);
    Serial.println("Continuing with assigned IP — update localIP in sketch if needed.");
  }
}

// Registration
// Sent once at startup. VTN responds confirming venID
void registerVEN() {
  Serial.println("\n--- Party Registration ---");
  String reqID = "req-" + String(requestSeq++);
  String body  = buildMessage(templPartyRegistration, reqID);

  String response = postToVTN(body, VTN_PATH_REGISTER);

  if (response.length() > 0) {
    Serial.println("Party registration response received:");
    Serial.println(response);
    registered = true;
    Serial.println("VEN registered. Device ID: " + String(VEN_ID));
    Serial.println();
    Serial.println(">>> Take screenshot now. Polling starts in 30 seconds...");
    delay(30000);
    Serial.println(">>> Starting poll loop.");
  } else {
    Serial.println("Registration failed - will retry on next poll.");
  }
}

// Poll
// Called every second. Sends oadrPoll, checks response for
// a pending oadrDistributeEvent.
void pollVTN() {
  String reqID = "req-" + String(requestSeq++);
  String msg   = buildMessage(templPoll, reqID);
  String response = postToVTN(msg, VTN_PATH_POLL);

  // Empty string means postToVTN failed — not a normal empty poll
  if (response.length() == 0) {
    pollFailCount++;
    Serial.print("Poll failed. Failure count: ");
    Serial.print(pollFailCount);
    Serial.print(" of ");
    Serial.println(MAX_POLL_RETRIES);
    if (pollFailCount >= MAX_POLL_RETRIES) {
      Serial.println("Max poll failures reached. Halting.");
      Serial.println("Reset the Opta to retry.");
      while (1);
    }
    return;
  }

  // Reset fail count on any successful HTTP response
  pollFailCount = 0;

  // Check what kind of response we got
  if (response.indexOf("oadrDistributeEvent") >= 0) {
    Serial.println("Event detected in response.");
    handleEvent(response);
  } else if (response.indexOf("oadrResponse") >= 0) {
    // Normal empty poll - VTN has no pending events
    Serial.println("Poll acknowledged - no pending events.");
  } else {
    // Unexpected response type - log it but don't treat as failure
    Serial.println("Unexpected response type:");
    Serial.println(response.substring(0, 200));
  }
}

// Event handler
// Parses signal level from oadrDistributeEvent, acknowledges
// it (echo), then acts and repots.
void handleEvent(const String& eventXML) {
  Serial.println("--- RAW EVENT XML ---");
  Serial.println(eventXML);
  Serial.println("--- END EVENT XML ---");
  // Extract request ID and event ID
  currentEventID       = extractValue(eventXML, "ei:eventID");
  String requestID     = extractRequestID(eventXML);
  
  Serial.println("Event ID: "   + currentEventID);
  Serial.println("Request ID: " + requestID);

  // Extract SIMPLE signal payload(0, 1, 2, or 3)
  // Signal payload is nested: 
  // <ei:signalPayload><ei:payloadFloat><ei:value>1</ei:value>...
  // Extract the inner ei:value inside ei:signalPayload
  String signalBlock = extractValue(eventXML, "ei:signalPayload");
  Serial.println("Signal block: " + signalBlock);
  String rawValue = extractValue(signalBlock, "ei:value");
  Serial.println("Raw signal value: '" + rawValue + "'");
  int signalLevel = rawValue.toInt();
  Serial.print("Parsed signal level: ");
  Serial.println(signalLevel);
  Serial.print("SIMPLE signal level: ");
  Serial.println(signalLevel);

  // Step 1 - Acknowledge (echo) the event
  acknowledgeEvent(currentEventID);

  // Step 2 - Act and report
  switch (signalLevel) {
    case 0:
      Serial.println("Signal 0: Normal operations. No shed.");
      sendReport(currentEventID, requestID, 0);
      break;
    case 1:
      Serial.println("Signal 1: Light shed. Reporting 3 kW.");
      sendReport(currentEventID, requestID, 3);
      break;
    case 2:
      Serial.println("Signal 2: Moderate shed. Reporting 10 kW.");
      sendReport(currentEventID, requestID, 10);
      break;
    case 3:
      Serial.println("Signal 3: Emergency shed. Reporting max.");
      sendReport(currentEventID, requestID, 15);
      break;
    default:
      Serial.println("Unknown signal level: " + String(signalLevel));
  }
}

// Acknowledge event (echo)
void acknowledgeEvent( const String& eventID) {
  Serial.println("Acknowledging event: " + eventID);

  String reqID  = "req-" + String(requestSeq++);
  String msg    = templCreatedEvent;
  msg.replace("{{VEN_ID}}",     VEN_ID);
  msg.replace("{{EVENT_ID}}", eventID);
  msg.replace("{{OPT_TYPE}}",  "optIn");
  msg.replace("{{REQUEST_ID}}", reqID);
  msg = wrapEnvelope(msg);

  String response = postToVTN(msg, VTN_PATH_EVENT);
  Serial.println("Acknowledge response: " + response);
}

// Send compliance report
void sendReport(const String& eventID, const String& requestID, int shedKW) {
  Serial.print("Sending report: shed = ");
  Serial.print(shedKW);
  Serial.println(" kW");

  // Extract the requestID from the last event response
  String rptID = "rpt-" + String(reportSeq++);
  String msg   = templUpdateReport;
  msg.replace("{{VEN_ID}}",       VEN_ID);
  msg.replace("{{REPORT_ID}}",    rptID);
  msg.replace("{{REPORT_VALUE}}", String(shedKW));
  msg = wrapEnvelope(msg);

  String response = postToVTN(msg, VTN_PATH_REPORT);
  Serial.println("Report response: " + response);
}

// HTTP POST helper
String postToVTN(const String& body, const char* path) {
  int attempts = 0;

  while (attempts < MAX_POST_RETRIES) {
    attempts++;
    Serial.println("postToVTN: attempt " + String(attempts) +
                   " of " + String(MAX_POST_RETRIES));
    Serial.println("postToVTN: connecting to " +
                   String(VTN_HOST) + ":" + String(VTN_PORT));

    // Print full request before sending
    Serial.println("--- REQUEST HEADERS ---");
    Serial.println("POST " + String(path) + " HTTP/1.1");
    Serial.println("Host: " + String(VTN_HOST) + ":" + String(VTN_PORT));
    Serial.println("Content-Type: application/xml");
    Serial.println("Content-Length: " + String(body.length()));
    Serial.println("--- REQUEST BODY ---");
    Serial.println(body);
    Serial.println("--- END REQUEST ---");

    http.beginRequest();
    http.post(path);
    http.sendHeader("Content-Type", "application/xml");
    http.sendHeader("Content-Length", body.length());
    http.beginBody();
    http.print(body);
    http.endRequest();

    Serial.println("postToVTN: request sent, waiting for response...");

    int statusCode = http.responseStatusCode();
    Serial.print("postToVTN: status code: ");
    Serial.println(statusCode);

    String response = http.responseBody();
    Serial.print("postToVTN: response length: ");
    Serial.println(response.length());

    if (statusCode == 200) {
      return response;
    }

    Serial.print("HTTP error: ");
    Serial.println(statusCode);

    if (attempts < MAX_POST_RETRIES) {
      Serial.println("Retrying in 1 second...");
      delay(1000);
    }
  }

  Serial.println("postToVTN: all retries exhausted.");
  return "";
}

// XML value extractor
// Extracts text content from <tag>value</tag>
// Simple string search - sufficient for well-formed responses.
String extractValue(const String& xml, const String& tag) {
  String open = "<" + tag + ">";
  String close = "</" + tag + ">";
  int start = xml.indexOf(open);
  if (start < 0) return "";
  start += open.length();
  int end = xml.indexOf(close, start);
  if (end < 0) return "";
  return xml.substring(start, end);
}

// Special request ID XML value extractor
// The request ID tag is used more than once, so
// this function must identify the correct parent structure.
String extractRequestID(const String& xml) {
  // Skip past the eiResponse block entirely
  String afterResponse = xml;
  int eiResponseEnd = xml.indexOf("</ei:eiResponse>");
  if (eiResponseEnd >= 0) {
    afterResponse = xml.substring(eiResponseEnd);
  }
  // Now extract requestID from what remains
  return extractValue(afterResponse, "requestID");
}// ============================================================
//  opta_ven.ino - Arduino Opta OpenADR 2.0b VEN
//  Test protocol:
//    1. Mount flash filesystem, load XML template on startup
//    2. Connect to WiFi (172.16.0.2 static)
//    3. Register with VTN (172.16.0.1:8080)
//    4. Poll VTN every 1 second
//    5. On SIMPLE signal 1 -> shed 3 kW, report back
//    6. On SIMPLE signal 2 -> shed 10 kW, report back
//
//  All XML is built in RAM from the template loaded at boot.
//  No XML file writes occur after startup
// ============================================================

#include <WiFi.h>
#include <ArduinoHttpClient.h>
#include <BlockDevice.h>
#include <LittleFileSystem.h>

// -- Configuration Section --------------------

// Network config
const char* WIFI_SSID = "Entwise";
const char* WIFI_PASSWORD = "TolkienTreeTest2"
IPAddress   STATIC_IP   (172, 16, 0, 2);
IPAddress   GATEWAY     (172, 16, 0, 1);
IPAdress    SUBNET      (255, 255, 255, 0);
const char* VTN_HOST    = "172.16.0.1";
const int   VTN_PORT    = 8080;
const char* VTN_PATH    = "/OpenADR2/Simple/2.0b";

// Device identity
const char* VEN_ID      = "1234";
const char* VTN_ID      = "test-vtn";

// Filesystem
BlockDevice*      bd = BlockDevice::get_default_instance();
LittleFileSystem  fs("fs");
const char*       TEMPLATE_PATH = "/fs/openadr_template.xml";

// Template sectoins - loaded once at boot
// Each section is one XML block from the template file.
// Stored as String so we can use replace() in RAM.
String templRegister;         // oadrRegisterReport
String templPoll;             // oadrPoll
String templCreatedEvent;     // oadrCreatedEvent
String templUpdateReport;     // oadrUpdateReport

// Runtime State
bool    registered      = false;
int     requestSeq      = 0;
int     reportSeq       = 0;
string  currentEventID  = "";

unsigned long lastPoll = 0;
const unsigned long POLL_INTERVAL = 1000;     // 1 second

// HTTP client
WiFiClient  wifi;
HttpClient  http(wifi, VTN_HOST, VTN_PORT);

// -- Setup Section ----------------------------
// Code here runs once

void setup() {
  Serial.begin(9600);
  while (!Serial);
  Serial.println("Opta OpenADR VEN starting...");

  mountFilesystem();
  loadTemplates();
  connectWiFi();
  registerVEN();
}

// --Main Program Loop-------------------------
// Code here runs continuously

void loop() {
  if (mills() - latPoll >= POLL_INTERVAL) {
    lastPoll = millis();
    pollVTN();
  }
}

// -- Function call definitions ----------------

// Filesystem
void mountFilesystem() {
  Serial.println("Mounting filesystem...");
  int err = fs.mount(bd);
  if (err) {
    Serial.println("No filesystem - formatting...");
    fs.reformat(bd);
  }
  Serial.println("Filesystem ready.")
}

// Load and print template into sections
// Reads the file once and extracts each named XML block into
// its own String. All subsequent XML building uses these
// in-RAM strings - no furhter file reads.
void loadTemplates() {
  Serial.print("Loading template from ")
  Serial.println(TEMPLATE_PATH);

  FILE* f = fopen(TEMPLATE_PATH, "r");
  if (!f) {
    Serial.println("ERROR: template file not found.");
    Serial.println("Run the upload utility first.");
    while(1)
  }

  String raw = "";
  char buf[128];
  while (fgets(buf, sizeof(buf), f)) {
    raw +=buf;
  }
  fclose(f);

  Serial.print("Template loaded. Size: ");
  Serial.print(raw.length());
  Serial.println(" bytes");

  // Extract each section between its opening and closing tags
  tmplRegister      = extractSection(raw, "oadrRegisterReport");
  tmplPoll          = extractSection(raw, "oadrPoll");
  tmplCreatedEvent  = extractSection(raw, "oadrCreatedEvent");
  tmplUpdateReport  = extractSection(raw, "oadrUpdateReport");

  Serial.println("Templates parsed into RAM sections:");
  Serial.println("  oadrRegisterReport  : " +
                 String(tmplRegister.length())      + " bytes");
  Serial.println("  oadrPoll            : " +
                 String(tmplPoll.length())          + " bytes");
  Serial.println("  oadrCreatedEvent    : " +
                 String(tmplCreatedEvent.length())  + " bytes");
  Serial.println("  oadrUpdateReport    : " +
                 String(tmplUpdateReport.length())  + " bytes");
}

// Extracts <tagName>...</tagName> block from larger string
// This is key to breaking apart a larger XML file into smaller parts
String extractSection(const String& src, const String& tag) {
  String open  = "<" + tag + ">";
  String close = "</" + tag + ">";
  int start = src.indexOf(open);
  int end   = src.indexOf(close);
  if (start < 0 || end < 0) {
    Serial.println("Warnign: section not found: " + tag);
    return "";
  }
  return src.substring(start, end + close.length());
}

// Token replacement - builds a message from a template
// Takes a template section, replaces all {{TOKEN}} placeholders,
// raps in the standard oadrPayload envelope, and returns as String.
String buildMessage(const String& tmpl,
                    const String & requestID) {
  String msg = tmpl;
  msg.replace("{{VEN_ID}}",     VEN_ID);
  msg.replace("{{VTN_ID}}",     VTN_ID);
  msg.replace("{{REQUEST_ID}}", requestID);
  return wrapEnvelope(msg)
}

// Wrap an XML body in the standard OpenADR payload envelope
String wrapEnvelope(const String& body) {
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\?>"
    "<oadrPayload "
      "xmlns:ei=\"thhp://docs.oasis-open.org/ns/emix/2011/06/ei\" "
      "xmlns=\"oadr2b\">"
      "<oadrSignedObject>" +
        body +
      "</oadrSignedObject>"
    "</oadrPayload>";
}

// WiFi
void connectWiFi() {
  Serial.pritn("Connecting to WiFi");
  WiFi.config(STATIC_IP, GATEWAY, SUBNET);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
}

// Registration
// Sent once at startup. VTN responds confirming venID
void registerVEN() {
  Serial.println("\n--- Registering VEN ---");
  String reqID  = "req-" + String(requestSeq++);
  String body   = buildMessage(tmplRegister, reqID);

  String response = postToVTN(body);

  if (response.length() > 0) {
    Serial.println("Registration response received:");
    Serial.println(response);
    registered = true;
    Serial.println("VEN registered. Device ID: " + String(VEN_ID));
  } else {
    Serial.println("Registration faailed - wil retry on next poll");
  }
}

// Poll
// Called every second. Sends oadrPoll, checks response for
// a pending oadrDistributeEvent.
void pollVTN() {
  String reqID = "req-" + String(requestSeq++);
  String msg   = buildMessage(tmplPoll, reqID);
  String response = postToVTN(msg);

  if (response.length() == 0) return;

  Serial.print("Poll #");
  Serial.print(requestSeq);
  Serial.println(" - response received.");

  if (response.indexOf("oadrDistributeEvent") >= 0) {
    Serial.println("Event detected in response.")
    handleEvent(response);
  }
}

// Event handler
// Parses signal level from oadrDistributeEvent, acknowledges
// it (echo), then acts and repots.
void handleEvent(const String& eventXML) {
  // Extract event ID
  currentEventID = extractValue(eventXML, "ei:eventID");
  Serial.println("Event ID: " + currentEventID);

  // Extract SIMPLE signal payload(0, 1, 2, or 3)
  int signalLevel = extractValue(
                      eventXML, "ei:signalPayload").toInt();
  Serial.print("SIMPLE signal level: ");
  Serial.println(signalLevel);

  // Step 1 - Acknowledge (echo) the event
  acknowledgeEvent(currentEventID);

  // Step 2 - Act and report
  switch (signalLevel) {
    case 0:
      Serial.println("Signal 0: Normal operations. No shed.");
      sendreport(currentEventID, 0);
      break;
    case 1:
      Serial.println("Signal 1: Light shed. Reporting 3 kW.");
      sendReport(currentEventID, 3);
      break;
    case 2:
      Serial.println("Signal 2: Moderate shed. Reporting 10 kW");
      sendReport(currentEventID, 10);
      break;
    case 3:
      Serial.println("Signal 3: Emergency shed. Reporting max.");
      sendReport(currentEventID, 15);
      break;
    default:
      Serial.println("Unknown signal level: " + String(signalLevel));
  }
}

// Acknowledge event (echo)
void acknowledgeEvent( const String& eventID) {
  Serial.println("Acknowledging event: " + eventID);

  String reqID  = "req-" + String(requestSeq++);
  String msg    = tmplCreatedEvent;
  msg.replace("{{VEN_ID}}",     VEN_ID);
  msg.replace("{{EVENT_ID}}", event_ID);
  msg.replace("{{OPT_TYPE}}",  "optIn");
  msg = wrapEnvelope(msg);

  String response = postToVTN(msg);
  Serial.println("Acknowledge response: " + response);
}

// Send compliance report
void sendReport(const String& eventID, int shedKW) {
  Serial.print("Sending report: shed = ");
  Serial.print(shedKW);
  Serial.println(" kW");

  String rptID = "rpt-" + String(reportSeq++);
  String msg   = tmplUpdateReport;
  msg.replace("{{VEN_ID}}",       VEN_ID);
  msg.replace("{{REPORT_ID}}",    rptID);
  msg.replace("{{REPORT_VALUE}}", String(shedKW));
  msg = wrapEnvelop(msg);

  String response = postToVTN(msg);
  Serial.println("Report response: " + response);
}

// HTTP POST helper
String postToVTN(const String& body) {
  http.beginRequest();
  http.post(VTN_PATH);
  http.sendHeader("Content-Type", "application/xml");
  http.sendHeader("Content_Length", body.length());
  http.beginBody();
  http.print(body);
  http.endRequest();

  int statusCode = http.responseStatusCode();
  String response = http.responseBody();

  if (statusCode != 200) {
    Serial.print("HttP error: ");
    Serial.println(statusCode);
    return "";
  }
  return response;
}

// XML value extractor
// Extracts text content from <tag>value</tag>
// Simple string search - sufficient for well-formed responses.
String extractValue(const String& xml, const String& tag) {
  String open = "<" + tag + ">";
  String close = "<" + tag + ">";
  int start = xml.indexOf(open);
  if (start < 0) return "";
  start += open.length();
  int end = xml.indexOf(close, start);
  if (end < 0) return "";
  return xml.substring(start, end);
}
