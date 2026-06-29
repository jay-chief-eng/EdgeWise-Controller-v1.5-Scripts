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

#include <WiFi.h>
#include <WiFiServer.h>
#include <BlockDevice.h>
#include <LittleFileSystem.h>
#include <MBRBlockDevice.h>

using namespace mbed;

// --Configuration Section --------------------

// MAC address
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

// WiFi Credentials
const char* WIFI_SSID     = "Trumbull";
const char* WIFI_PASSWORD = "xcelsior97";

// Static IP configuration
IPAddress localIP     (10, 0, 0, 101);
IPAddress gateway     (10, 0, 0, 1);
IPAddress subnet      (255, 255, 255, 0);
IPAddress dns         (8, 8, 8, 8);

const int HTTP_PORT = 8888;

// --Filesystem-------------------------------
BlockDevice*      bd = BlockDevice::get_default_instance();
MBRBlockDevice    sys_bd(bd, 1);    // partition 1 - system/Wifi
MBRBlockDevice    usr_bd(bd, 2);    // partition 2 - user data
LittleFileSystem  fs("fs");

WiFiServer  server(HTTP_PORT);

// -- Setup Section --------------------------
// Code here runs once

void setup() {
  Serial.begin(9600); 
  while (!Serial);
  Serial.println("Opta File Upload Utility");
  Serial.println("========================");

  // Start File reading
  mountFilesystem();

  // WiFi
  
  WiFi.config(localIP, gateway, subnet, dns);
  connectWiFi();

  server.begin();
  Serial.println("HTTP server started on port " +
                 String(HTTP_PORT));
  Serial.println();

  // Delete old version of the file if present
  deleteFile("/fs/openadr_template.xml");
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
    handleUpload(client, request);
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
  String body = request.substring(bodystart + 4);

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
  fprintf(f, "%s", body.c_str());
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
  sendResponse(client, 200, msg);

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
    filename = filename.substring(1);
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
String extractHeader(const String& request,
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

// Wifi Connect
void connectWiFi() {
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

// Delete old versions of the file

void deleteFile(const char* path) {
  Serial.print("Deleting: ");
  Serial.println(path);
  int result = remove(path);
  if (result == 0) {
    Serial.println("Deleted successfully.");
  } else {
    Serial.println("Delete failed — file may not exist.");
  }
}
