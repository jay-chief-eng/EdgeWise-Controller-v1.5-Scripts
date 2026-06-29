#include <WiFi.h>
#include <ArduinoMqttClient.h>

// --Configuration section---------------------

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

// Broker configuration
const char* BROKER_IP   = "10.0.0.100";
const int   BROKER_PORT = 1883;

// Topics
const char* TOPIC_STATUS        = "opta/status";
const char* TOPIC_DEVICE_ID     = "opta/device-id";
const char* TOPIC_HEARTBEAT     = "opta/heartbeat";
const char* TOPIC_CMD_INPUT     = "opta/commands/input";
const char* TOPIC_CMD_ECHO      = "opta/commands/echo";
const char* TOPIC_CMD_RESULT    = "opta/commands/result";

// MQTT client
WiFiClient  wifiClient;
MqttClient  mqttClient(wifiClient);

// Heartbeat variable definitions
int           heartbeatCounter  = 1;
unsigned long lastHeartbeatTime  = 0;
const long    HEARTBEAT_INTERVAL = 1000;    // units are ms

// --Setup section----------------------------- 
// Code here runs once

void setup() {
  Serial.begin(115200);
  while (!Serial);

  Serial.println("Program has begun.");

  //Configure static IP before connecting
  WiFi.config(localIP, gateway, subnet, dns);

  //Connect
  connectWiFi();
  connectMQTT();

}

// --Main Program Loop-------------------------
// Code here runs continuously

void loop() {
  // Reconnect if connection is lost
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!mqttClient.connected()) {
    connectMQTT();
  }

  // Must be called regularly to process incoming messages
  mqttClient.poll();

  // Heartbeat - publish once per second
  unsigned long now = millis();
  if (now - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
    lastHeartbeatTime = now;
    publishHeartbeat();
  }
}

// -- Function call definitions ---------------

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

// MQTT connection helper function
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

  // Publish device ID once on connect
  mqttClient.beginMessage(TOPIC_DEVICE_ID, true);
  mqttClient.print(1234);
  mqttClient.endMessage();
  Serial.println("Device ID published: 1234");

  // Subscribe to incoming commands
  mqttClient.subscribe(TOPIC_CMD_INPUT);
  mqttClient.onMessage(onCommandReceived);
  Serial.println("Subscribed to opta/commands/input");
}

// Heartbeat helper function
void publishHeartbeat() {
  mqttClient.beginMessage(TOPIC_HEARTBEAT);
  mqttClient.print(heartbeatCounter);
  mqttClient.endMessage();

  Serial.print("Heartbeat: ");
  Serial.println(heartbeatCounter);

  heartbeatCounter++;
  if (heartbeatCounter > 10) {
    heartbeatCounter = 1;
  }
}

// Command processing function
void onCommandReceived(int messageSize) {
  // Read the incoming command value
  String payload = "";
  while (mqttClient.available()) {
    payload += (char)mqttClient.read();
  }

  // Convert the command from string to int for processing
  int commandValue = payload.toInt();
  Serial.print("Command received: ");
  Serial.println(commandValue);

  // Echo the command back
  mqttClient.beginMessage(TOPIC_CMD_ECHO);
  mqttClient.print(commandValue);
  mqttClient.endMessage();

  // Publish command * 2
  mqttClient.beginMessage(TOPIC_CMD_RESULT);
  mqttClient.print(commandValue * 2);
  mqttClient.endMessage();

  Serial.print("Echo: ");
  Serial.print(commandValue);
  Serial.print(" Result: ");
  Serial.println(commandValue * 2);
}#include <WiFi.h>
#include <ArduinoMqttClient.h>

// --Configuration section---------------------

// WiFi Credentials
const char* WIFI_SSID     = "Entwise";
const char* WIFI_PASSWORD = "TolkienTreeTest2"

// Static IP configuration
IPAddress localIP(172, 16, 0, 2);
IPaddress gateway(172, 16, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8)

// Broker configuration
const char* BROKER_IP   = "172.16.0.1";
const int   BROKER_PORT = 1883;

// Topics
const char* TOPIC_STATUS        = "opta/status";
const char* TOPIC_DEVICE_ID     = "opta/device-id";
const char* TOPIC_HEARTBEAT     = "opta/heartbeat";
const char* TOPIC_CMD_INPUT     = "opta/commands/input";
const char* TOPIC_CMD_ECHO      = "opta/commands/echo";
const char* TOPIC_CMD_RESULT    = "opta/commands/result";

// MQTT client
WiFiClient  wifiClient;
MqttClient  mqttClient(wifiClient);

// Heartbeat variable definitions
int           heartbeatCounter  = 1;
unsigned long lastHearbeatTime  = 0;
const long    HEARTBEAT_INTERVAL = 1000;    // units are ms

// --Setup section----------------------------- 
// Code here runs once

void setup() {
  Serial.begin(115200;
  while (!Serial);

  //Configure static IP before connecting
  if (!WiFi.config(localIP, gateway, subnet, dns)) {
    Serial.println("Static IP Configuration failed");
  }

  connectWiFi();
  connectMQTT();

}

// --Main Program Loop-------------------------
// Code here runs continuously

void loop() {
  // Reconnect if connection is lost
  if (!WiFi.isConnected()) {
    connectWiFi();
  }
  if (!mqttClient.connected()) {
    connectMQTT();
  }

  // Must be called regularly to process incoming messages
  mqttClient.poll();

  // Heartbeat - publish once per second
  unsigned long now = millis();
  if (now - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
    lastHeartbeatTime = now;
    publishHeartbeat();
  }
}

// -- Function call definitions ---------------

// WiFi connection helper function
void connectWiFi() {
  Serial.print("Connecting to WiFi");
  Wifi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (!WiFi.isConnected()) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.print("WiFi connected - IP: ");
  Serial.println(WiFi.localIP());
}

// MQTT connection helper function
void connectMQTT() {
  // Last Will: broker publishes "offline" to opta/status if we drop
  mqttClient.beginWill(TOPIC_STATUS, true, 1);
  mqttClient.print("offline");
  mqttClient.endWill();

  Serial.print("Connecting to MQTT broker");
  while (!mqttClient.connect(BROKER_IP, BROKER_PORT)) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.println("MQTT connected");

  // Announce presence
  mqttClient.beginMessage(TOPIC_STATUS, true);    // retain = true
  mqttClient.print("online");
  mqttClient.endMessage();
  Serial.println("Announced presence by setting presence to online");

  // Publish device ID once on connect
  mqttClient.beginMessage(TOPIC_DEVICE_ID, true);
  mqttClient.print(1234);
  mqttClient.endMessage();
  Serial.println("Device ID published: 1234");

  // Subscribe to incoming commands
  mqtt.Client.subscribe(TOPIC_CMD_INPUT);
  mqttClient.onMessage(onCommandReceived);
  Serial.println("Subscribed to opta/commands/input");
}

// Heartbeat helper function
void publishHeartbeat() {
  mqttClient.beginMessage(TOPIC_HEARTBEAT);
  mqttClient.print(heartbeatCounter);
  mqttClient.endMessage();

  Serial.print("Heartbeat: ");
  Serial.println(heartbeatCounter);

  heartbeatCounter++;
  if (heartbeatCounter > 10) {
    heartbeatCounter = 1;
  }
}

// Command processing function
void onCommandReceived(int messageSize) {
  // Read the incoming command value
  String payload = "";
  while (mqttClient.available()) {
    payload += (char)mqttClient.read();
  }

  // Convert the command from string to int for processing
  int commandValue = payload.toInt();
  Serial.print("Command received: ");
  Serial.println(commandValue);

  // Echo the command back
  mqttClient.beginMessage(TOPIC_CMD_ECHO);
  mqttClient.print(commandValue);
  mqttClient.endMessage();

  // Publish command * 2
  mqttClient.beginMessage(TOPIC_CMD_RESULT);
  mqttClient.print(commandValue * 2);
  mqttClient.endMessage();

  Serial.print("Echo: ");
  Serial.print(commandValue);
  Serial.print(" Result: ");
  Serial.println(commandValue * 2);
}
