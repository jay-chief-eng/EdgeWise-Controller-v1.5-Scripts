#include <ArduinoRS485.h>
#include <ArduinoModbus.h>
#include <SPI.h>
#include <Ethernet.h>

// -- Configuration Section --------------------

// Network Config
byte mac[]        = { 0xDE, 0xAD, 0xEF, 0xFE, 0xED };
IPAddress ip      (172, 16, 0, 2);        // Opta static IP
IPAddress server  (172, 16, 0, 1);        // Laptop IP
const int MODBUS_TCP_PORT = 502;

// Setting up the Modbus client
EthernetClient ethClient;
ModbusTCPClient modbusTCPClient(ethClient);

// Modbus register indices
const int HOLDING_BASE    = 100;
const int HR_COMMAND      = 101;          // Laptop writes command here
const int HR_CMD_ECHO     = 102;          // Opta echoes command here
const int HR_CMD_X2       = 103;          // Opta writes command x 2 here
const int INPUT_BASE      = 100;
const int IR_COUNTER      = 100;          // Opta writes counter here
const int IR_DEVICE_ID    = 101;          // Opta write device ID here

// Timing
unsigned long lastCounterUpdate = 0;
unsigned long lastPoll          = 0;
const unsigned long COUNTER_INTERVAL = 1000;  // 1 second
const unsigned long POLL_INTERVAL    = 500;   // poll every 500 ms

int counter = 1;

// -- Setup Section --------------------
// Code here runs once

// Mode selection
// Comment one out to select mode
#define USE_MODBUS_TCP
// #define USE_MODBUS_RTU

void setup() {
  Serial.begin(9600);
  while (!Serial);
  Serial.println("Opta Modbus Test Starting...");

#ifdef USE_MODBUS_TCP
  setupTCP();
#endif

#ifdef USE_MODBUS_RTU
  setupRTU();
#endif
}

// TCP Setup
void setupTCP() {
  Ethernet.begin(mac, ip);
  delay(1000);
  Serial.print("Opta IP: ");
  Serial.println(Ethernet.localIP());

  if (!modbusTCPClient.begin(server, MODBUS_TCP_PORT)) {
    Serial.println("Failed to connect to Modbus TCP server. Halting.");
    while (1);
  }
  Serial.println("Modbus TCP connected.");

  // Read device ID from input register 100
  // Pre-set this to 1234 in ModbusPal before running
  int deviceID = modbusTCPClient.inputRegisterRead(IR_DEVICE_ID);
  if (deviceID < 0) {
    Serial.println("Failed to read device ID.");
  } else {
    Serial.print("Device ID read from IR 100: ");
    Serial.println(deviceID);
  }
}

// RTU Setup
void setupRTU() {
  // Default device address 1, 9600 baud, 8N1
  if (!ModbusRTUClient.begin(9600)) {
    Serial.println("Failed to start Modbus RTU. Halting");
    while (1);
  }
  Serial.println("Modbus RTU started.");

  // Read device ID from input register 100
  // Pre-set this to 1234 in ModbusPal before running
  int deviceID = modbusTCPClient.inputRegisterRead(IR_DEVICE_ID);
  if (deviceID < 0) {
    Serial.println("Failed to read device ID.");
  } else {
    Serial.print("Device ID read from IR 100: ");
    Serial.println(deviceID);
  }
}

// --Main Program Loop-------------------------
// Code here runs continuously

void loop() {
  unsigned long now = millis();

  // Update counter every second
  if (now - lastCounterUpdate >= COUNTER_INTERVAL) {
    lastCounterUpdate = now;
    writeInputRegister(IR_COUNTER, counter);
    Serial.print("Counter: ");
    Serial.println(counter);
    counter++;
    if (counter > 10) counter = 1;
  }

  // Poll command register and respond
  if (now - lastPoll >= POLL_INTERVAL) {
    lastPoll = now;

    int command = readHoldingRegister(HR_COMMAND);
    if (command >= 0) {
      Serial.print("Command received: ");
      Serial.println(command);

      writeHoldingRegister(HR_CMD_ECHO, command);     // echo
      writeHoldingRegister(HR_CMD_X2,   command * 2); // x2
    }
  }
}

// -- Function call definitions -----------------------
// Modbus Register helpers
// These wrap TCP and RTU behind a common interface so the
// loop() code doesn't need to care which mode is active.

int readHoldingRegister(int index) {
  #ifdef USE_MODBUS_TCP
    int val = modbusTCPClient.holdingRegisterRead(index);
    if (val < 0) Serial.println("HR read error");
    return val;
  #endif
  #ifdef USE_MODBUS_RTU
    int val = ModbusRTUClient.holdingRegisterRead(1, index);
    if (val < 0) Serial.println("HR read error");
    return val;
  #endif
}

void writeHoldingRegister(int index, int value) {
  #ifdef USE_MODBUS_TCP
    modbusTCPClient.holdingRegisterWrite(index, value);
  #endif
  #ifdef USE_MODBUS_RTU
    ModbusRTUClient.holdingRegisterWrite(1, index, value);
  #endif
}

void writeInputRegister(int index, int value) {
  // Input registers are read-only from the client perspective - 
  // in a real deployment the meter owns these.
#ifdef USE_MODBUS_TCP
  modbusTCPClient.inputRegisterRead(index, value);
#endif
#ifdef USE_MODBUS_RTU
  ModbusRTUClient.inputRegisterRead(1, index, value);
#endif
}
