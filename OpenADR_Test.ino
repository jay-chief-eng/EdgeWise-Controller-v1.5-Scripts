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