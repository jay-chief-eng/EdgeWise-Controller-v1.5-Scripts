// ============================================================
//  upload_template.ino - Standalone file upload utility
//
//  PURPOSE:
//    This sketch starts a small HTTP server on the Opta that accepts
//    a file upload from the laptop, writes it to flash, then
//    confirms success via Serial and HTTP response.
//
//  INSTRUCTIONS FOR OPENADR TEST
//    Run this sketch ONCE before deploying opta_ven.ino.
//    Once the file upload is confirmed, start opta_ven.ino and the
//    VEN sketch starts with the XML templates already present on 
//    flash.
//
//   LAPTOP SIDE:
//    With the Opta running this sketch at 172.16.0.2, run
//    this curl command from your terminal:
//
//      curl -X POST http://172.16.0.2:8888 \
//           -H "X-Filename: openadr_template.xml" \
//           --data-binary @openadr_template.xml
//
//    Note: the file openadr_template.xml must be present in the 
//    same file level where you issue the command for the upload.
//    Otherwise, you must provide the full path to the file.
//
//    You should see "Upload OK: /fs/openadr_template.xml"
//    in both the curl output and the Arduino Serial Monitor.
//
//   VERIFICATION:
//    After uploading, this sketch also accepts a GET request 
//    to read the file back for confirmation:
//
//      curl http://172.16.0.2:8888/openadr_template.xml
//
// ============================================================

#include <WiFi.h
#include <WiFiServer.h>
#include <BlockDevice.h>
#include <LittleFileSystem.h>

// --Configuration Section --------------------

// --Network config ---------------------------
const char* WIFI_SSID = "Entwise";
const char* WIFI_PASSWORD = "TolkienTreeTest2"
IPAddress   STATIC_IP   (172, 16, 0, 2);
IPAddress   GATEWAY     (172, 16, 0, 1);
IPAdress    SUBNET      (255, 255, 255, 0);
const int   HTTP_PORT   = 8888;

// --Filesystem-------------------------------
BlockDevice*      bd = BlockDevice::get_default_instance();
LittleFileSystem  fs("fs");

WiFiServer  server(HTTP_PORT);

// -- Setup Section --------------------------
// Code here runs once

void setup() {
  Serial.begin(9600); 
  while (!Serial);
  Serial.println("Opta File Upload Utility");
  Serial.println("========================");

  mountFilesystem();
  connectWiFi();

  server.begin();
  Serial.println("HTTP server started on port " +
                 String(HTTP_PORT));
  Serial.println();
}

// --Main Program Loop-------------------------
// Code here runs continuously

void loop() {
  WiFiClient client = server.available();
  if (!client) return;

  Serial.println("Client connected.");

  // Read the full HTTP request into a String.
  // We wait briefly for all data to arrive.
  String request = "";
  unsigned long timeout = millis() + 3000;
  while (client.connected() && millis() < timeout) {
    while (client.available()) {
      request += (char)client.read();
      timeout = millis() + 200;   // reset timeout on each byte
    }
  }

  Serial.println("Request received (" +
                  String(request.length()) + " bytes)");

  // Route: GET /filename - read file back
  if (request.startsWith("GET")) {
    handleRead(client, request);
  }
  // Route: Post / - write file
  else if (request.startsWith("POST")) {
    handlUpload(client, request);
  }
  else {
    sendResponse(client, 405, "Method not allowed");
  }

  client.stop();
  Serial.println("Client disconnected.");
}

// -- Function call definitions -----------------------

// Handle file upload (POST)
void handleUpload(WiFiClient& client, const String& request) {

  // Extract filename from X-Filename header
  String filename = extractHeader(request, "X-Filename");
  if (filename.length() == 0) {
    sendResponse(client, 400,
    "Missing X-Filename header.\n"
    "Usage: curl -H \"X-Filename: openadr_template.xml\" ...");
    return;
  }

  // Extract body (content after blank line separating headers)
  int bodystart = request.indexOf("\r\n\r\n");
  if (bodystart < 0) {
    sendResponse(client, 400, "Malformed request - no body.");
    return;
  }
  String body = request.substring(bodyStart + 4);

  if (body.length() == 0) {
    sendResponse(client, 400, "Empty file body.");
    return;
  }

  //Write to flash
  String path = "/fs/" + filename;
  Serial.print("Writing to " + path + " ... ");

  FILE* f = fopen(path.c_str(), "w");
  if (!f) {
    Serial.println("FAILED");
    sendResponse(client, 500, "Could not open file for writing.");
    return;
  }
  fprintf(f, %s, body.c_str());
  fclose(f);

  Serial.println("OK (" + String(body.length()) + " bytes)");

  // Verify by reading back and checking size
  FILE* v = fopen(path.c_str(), "r");
  int verifySize = 0;
  if (v) {
    fseek(v, 0, SEEK_END);
    verifySize = ftell(v);
    fclose(v);
  }

  String msg = "Upload OK: " + path + "\n" +
               "Written : " + String(body.length()) + "bytes\n" +
               "Verified: " + String(verifySize) + " bytes on flash\n";

  Serial.println(msg);
  sendResponse(client, 200, msgg);

  // List all files so operator can confirm
  listFiles();
}

// Handle file read (GET)
void handleRead(WiFiClient& client, const String& request) {

  // Extract path from GET /filename HTTP/1.1
  int pathStart = 5;    // after "Get/"
  int pathEnd   = request.indexOf(" ", pathStart);
  String filename = request.substring(pathStart, pathEnd);

  // Remove leading slash if present
  if (filename.startsWith("/")) {
    filename = silename.substring(1);
  }

  String path = "/fs/" + filename;
  Serial.println("Reading: " + path);

  FILE* f = fopen(path.c_str(), "r");
  if (!f) {
    sendResponse(client, 404, "File not found: " + path);
    return;
  }

  String content = "";
  char buf[128];
  while (fgets(buf, sizeof(buf), f)) {
    content += buf;
  }
  fclose(f);

  client.println("HTTP/1.1 200 OK");
  client.println("Content - Type: application/xml");
  client.println("Content-Length: " + String(content.length()));
  client.println("Connection: close");
  client.println();
  client.print(content);
}

// HTTP response helper
void sendResponse(WiFiClient& client, int code,
                  const String & body) {
  String status = (code == 200) ? "OK" :
                  (code == 400) ? "Bad Request" :
                  (code == 404) ? "Not Found" :
                  (code == 405) ? "Method Not Allowed" :
                                  "Internal Server Error";
  client.println("HTTP/1.1 " + String(code) + " " + status);
  client.println("Content-Type: text/plain");
  client.println("Content-Length: " + String(body.length()));
  client.println("Connection: close");
  client.println();
  client.print(body);
}

// Extract a header value from raw HTTP request
String extractHeader(const String& reqeust,
                     const String& headerName) {
  String search = headerName + ": ";
  int start = request.indexOf(search);
  if (start < 0) return "";
  start += search.length();
  int end = request.indexOf("\r\n", start);
  if (end < 0) return "";
  return request.substring(start, end);
}

// List files on flash to Serial
void listFiles() {
  Serial.println("\nFiles on /fs/:");
  DIR* dir = opendir("/fs");
  if (!dir) { Serial.println("  (could not open directory)"); return; }
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    String fullPath = "/fs/" + String(entry->d_name);
    struct stat st;
    stat(fullPath.c_str(), &st);
    Serial.println(" " + String(entry->d_name) +
                   " (" + String(st.st_size) + " bytes)");
  }
  closedir(dir);
  Serial.println();
}

// Filesystem mount
void mountFilesystem() {
  Serial.print("Mountin filesystem... ");
  int err = fs.mount(bd);
  if (err) {
    Serial.println("not found. Formatting...");
    fs.reformat(bd);
  } else {
    Serial.println("OK");
  }
}

// Wifi Connect
void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.config(STATIC_IP, GATEWAY, SUBNET);
  WiFi.begin(WIFI_SSID), WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected. IP: ");
  Serial.println(WiFi.localIP());
}