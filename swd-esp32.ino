#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <EEPROM.h>
#include <stdint.h> 

#define PUMP_PIN 4
#define SENSOR_ANALOG_PIN 12
#define SENSOR_POWER_PIN 14
#define LED_PIN 13

unsigned long savedTime = 0;

//save data each hour
unsigned long lastSaveTime = 0;
const unsigned long saveInterval = 3600000;

// Standard BLE UUIDs for environmental sensing
#define ENVIRONMENTAL_SENSING_SERVICE_UUID BLEUUID((uint16_t)0x181A)
#define SENSOR_DATA_CHARACTERISTIC_UUID BLEUUID((uint16_t)0x2A6E)
#define COMMAND_CHARACTERISTIC_UUID BLEUUID((uint16_t)0x2A3D) 
#define MAX_LEVEL_CHARACTERISTIC_UUID BLEUUID((uint16_t)0x2A3E)
#define MIN_LEVEL_CHARACTERISTIC_UUID BLEUUID((uint16_t)0x2A3F) 
#define HISTORY_CHARACTERISTIC_UUID BLEUUID((uint16_t)0x2A87)

BLEServer* pServer = NULL;
BLECharacteristic* pSensorCharacteristic = NULL;
BLECharacteristic* pCommandCharacteristic = NULL;
BLECharacteristic* pMaxLevelCharacteristic = NULL;
BLECharacteristic* pMinLevelCharacteristic = NULL;
BLECharacteristic* pHistoryCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// Generate a unique device ID or read it from EEPROM
String deviceID = "";
#define EEPROM_SIZE 128
#define ID_LENGTH 4 

// EEPROM address definitions
#define MODE_ADDRESS 4
#define MAX_LEVEL_ADDRESS (MODE_ADDRESS + 8)
#define MIN_LEVEL_ADDRESS (MAX_LEVEL_ADDRESS + sizeof(int))

#define SENSOR_DATA_START  110         
#define MAX_SENSOR_ENTRIES 200        
#define SENSOR_DATA_INDEX  100

// Global const variables for max and min levels
int currentMaxLevel = 0;
int currentMinLevel = 0;
String currentMode = "";

void updateLevelCharacteristics();

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("Device connected");
      
      // Update characteristic values when a device connects
      updateLevelCharacteristics();
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("Device disconnected");
    }
};

void processCommand(String command);

// Callback class for handling incoming data
class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = pCharacteristic->getValue();

    if (rxValue.length() > 0) {
      Serial.println("*********");
      Serial.print("Received command: ");
      Serial.println(rxValue);

      processCommand(rxValue);

      Serial.println("*********");
    }
  }
};

// Update the BLE characteristics with current values
void updateLevelCharacteristics() {
  pMaxLevelCharacteristic->setValue(String(currentMaxLevel).c_str());
  pMinLevelCharacteristic->setValue(String(currentMinLevel).c_str());
  
  Serial.println("Updated BLE characteristics with current levels:");
  Serial.print("MAX_LEVEL: ");
  Serial.println(currentMaxLevel);
  Serial.print("MIN_LEVEL: ");
  Serial.println(currentMinLevel);
}

// Function to process commands received from the app
void processCommand(String command) {
  digitalWrite(LED_PIN, HIGH);
  delay(100);
  digitalWrite(LED_PIN, LOW);


  if (command == "PUMP_ON") {
    Serial.println("Turning PUMP ON");
    digitalWrite(PUMP_PIN, HIGH);
  } 
  else if (command == "PUMP_OFF") {
    Serial.println("Turning PUMP OFF");
    digitalWrite(PUMP_PIN, LOW);
  }
  else if (command.startsWith("SET_INTERVAL:")) {
    int interval = command.substring(13).toInt();
    Serial.print("Setting interval to: ");
    Serial.println(interval);
  }
  else if (command == "AUTO") {
    Serial.println("Setting to AUTO mode");
    setMode("AUTO");
    currentMode = "AUTO";
  }
  else if (command == "MANUAL") {
    Serial.println("Setting to MANUAL mode");
    setMode("MANUAL");
    currentMode = "MANUAL";
    // turn off pump
    digitalWrite(PUMP_PIN, LOW);
  }
  else if (command.startsWith("SET_MAX_LEVEL:")) {
    int maxLevel = command.substring(14).toInt();
    Serial.print("Setting max level to: ");
    Serial.println(maxLevel);
    setNewMaxLevel(maxLevel);
    currentMaxLevel = maxLevel;
    updateLevelCharacteristics();
  }
  else if (command.startsWith("SET_MIN_LEVEL:")) {
    int minLevel = command.substring(14).toInt();
    Serial.print("Setting min level to: ");
    Serial.println(minLevel);
    setNewMinLevel(minLevel);
    currentMinLevel = minLevel; 
    updateLevelCharacteristics();
  }

  else if (command == "GET_HISTORY_DATA") {
    Serial.println("Requesting history data");
    sendHistoryData(72);
  }
  else {
    Serial.print("Unknown command: ");
    Serial.println(command);
  }
}

void initializeSettings() {
  // Check if mode has been set
  currentMode = getMode();
  if (currentMode.length() == 0) {
    setMode("MANUAL");
    currentMode = "MANUAL";
  }
  
  // Check if max level has been set (a value of -1 would indicate uninitialized)
  currentMaxLevel = getMaxLevel();
  if (currentMaxLevel <= 0) {
    setNewMaxLevel(1000);
    currentMaxLevel = 1000;
  }
  
  // Check if min level has been set
  currentMinLevel = getMinLevel();
  if (currentMinLevel <= 0) {
    setNewMinLevel(200);
    currentMinLevel = 200;
  }
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  
  // Check if we have a stored ID
  if (!readDeviceID()) {
    generateDeviceID();
    saveDeviceID();
  }

  // Initialize settings
  initializeSettings(); 
  Serial.println("Current mode: " + currentMode);
  Serial.print("Max level: ");
  Serial.println(currentMaxLevel);
  Serial.print("Min level: ");
  Serial.println(currentMinLevel);
  
  Serial.print("Device ID: ");
  Serial.println(deviceID);
  
  // Create the BLE Device with the ID in the name
  String deviceName = "ESP32_" + deviceID;
  BLEDevice::init(deviceName.c_str());
  
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  BLEService *pService = pServer->createService(ENVIRONMENTAL_SENSING_SERVICE_UUID);
  
  // Create a BLE Characteristic for sensor data 
  pSensorCharacteristic = pService->createCharacteristic(
                      SENSOR_DATA_CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  
  // Add a descriptor to the sensor characteristic
  pSensorCharacteristic->addDescriptor(new BLE2902());
  
  // Create a new characteristic for receiving commands
  pCommandCharacteristic = pService->createCharacteristic(
                      COMMAND_CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_WRITE
                    );
  
  pCommandCharacteristic->setCallbacks(new CommandCallbacks());
  
  // Create characteristics for reading current MAX_LEVEL and MIN_LEVEL values
  pMaxLevelCharacteristic = pService->createCharacteristic(
                      MAX_LEVEL_CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ
                    );
  
  pMinLevelCharacteristic = pService->createCharacteristic(
                      MIN_LEVEL_CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ
                    );
  
  // Create a characteristic for reading history data
  pHistoryCharacteristic = pService->createCharacteristic(
                      HISTORY_CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  pHistoryCharacteristic->addDescriptor(new BLE2902());

  // Set initial values for the level characteristics
  pMaxLevelCharacteristic->setValue(String(currentMaxLevel).c_str());
  pMinLevelCharacteristic->setValue(String(currentMinLevel).c_str());


  pService->start();
  
  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(ENVIRONMENTAL_SENSING_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06); 
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("Advertising started. Device is ready to connect");
  
  // Set up pins for outputs
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(SENSOR_POWER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  // set led off
  digitalWrite(LED_PIN, LOW);

}

bool readDeviceID() {
  char storedID[ID_LENGTH + 1]; // +1 for null terminator
  
  // Read the ID from EEPROM
  for (int i = 0; i < ID_LENGTH; i++) {
    storedID[i] = EEPROM.read(i);
  }
  
  storedID[ID_LENGTH] = '\0';
  
  // Check if the stored ID is valid (not all 0xFF which is the default EEPROM value)
  if (storedID[0] != 0xFF) {
    deviceID = String(storedID);
    return true;
  }
  return false;
}

void saveDeviceID() {
  // Write ID to EEPROM byte by byte
  for (int i = 0; i < ID_LENGTH; i++) {
    if (i < deviceID.length()) {
      EEPROM.write(i, deviceID[i]);
    } else {
      // Pad with nulls if ID is shorter than ID_LENGTH
      EEPROM.write(i, '\0');
    }
  }
  
  EEPROM.commit();
  Serial.println("Device ID saved to EEPROM: " + deviceID);
}

void generateDeviceID() {
  const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  deviceID = "";
  
  // Generate exactly ID_LENGTH characters
  for (int i = 0; i < ID_LENGTH; i++) {
    int index = random(0, strlen(charset));
    deviceID += charset[index];
  }
  
  Serial.println("Generated new device ID: " + deviceID);
}

void setNewMaxLevel(int newMaxLevel) {
  // Store the max level value in EEPROM
  EEPROM.put(MAX_LEVEL_ADDRESS, newMaxLevel);
  EEPROM.commit();
  Serial.print("New max level saved to EEPROM: ");
  Serial.println(newMaxLevel);
}

void setNewMinLevel(int newMinLevel) {
  // Store the min level value in EEPROM
  EEPROM.put(MIN_LEVEL_ADDRESS, newMinLevel);
  EEPROM.commit();
  Serial.print("New min level saved to EEPROM: ");
  Serial.println(newMinLevel);
}

void setMode(String mode) {
  // led blinking indication when setMode was called
  digitalWrite(LED_PIN, HIGH);
  delay(100);
  digitalWrite(LED_PIN, LOW);
  delay(100);
  digitalWrite(LED_PIN, HIGH);
  delay(100);
  digitalWrite(LED_PIN, LOW);



  // Store the mode string in EEPROM
  // First clear the previous value
  for (int i = 0; i < 8; i++) {
    EEPROM.write(MODE_ADDRESS + i, 0);
  }
  
  // Write the new mode string
  for (int i = 0; i < mode.length() && i < 8; i++) {
    EEPROM.write(MODE_ADDRESS + i, mode[i]);
  }
  
  EEPROM.commit();
  Serial.print("New mode saved to EEPROM: ");
  Serial.println(mode);
}

String getMode() {
  String mode = "";
  for (int i = 0; i < 8; i++) {
    char c = EEPROM.read(MODE_ADDRESS + i);
    if (c != 0) {
      mode += c;
    } else {
      break;
    }
  }
  return mode;
}

int getMaxLevel() {
  int maxLevel;
  EEPROM.get(MAX_LEVEL_ADDRESS, maxLevel);
  return maxLevel;
}

int getMinLevel() {
  int minLevel;
  EEPROM.get(MIN_LEVEL_ADDRESS, minLevel);
  return minLevel;
}

void saveDataFromSensor(int16_t data) {
  int index;

  // Read current index
  EEPROM.get(SENSOR_DATA_INDEX, index);

  // Sanity check and wrap around
  if (index < 0 || index >= MAX_SENSOR_ENTRIES) {
    index = 0;
  }

  // Calculate address to store the next int16_t
  int address = SENSOR_DATA_START + index * sizeof(int16_t);
  EEPROM.put(address, data);

  // Update index and wrap if necessary
  index++;
  if (index >= MAX_SENSOR_ENTRIES) {
    index = 0;
  }

  // Save updated index back
  EEPROM.put(SENSOR_DATA_INDEX, index);
  EEPROM.commit();

  Serial.print("Saved sensor value ");
  Serial.print(data);
  Serial.print(" at EEPROM address ");
  Serial.println(address);
}



//read the saved data from EEPROM
String readSensorHistory(int count) {
  String historyData = "";
  int currentIndex;
  
  // Read current write index
  EEPROM.get(SENSOR_DATA_INDEX, currentIndex);
  
  // Sanity check
  if (currentIndex < 0 || currentIndex >= MAX_SENSOR_ENTRIES) {
    currentIndex = 0;
  }
  
  // Calculate how many entries to read (limited by MAX_SENSOR_ENTRIES)
  int readCount = min(count, MAX_SENSOR_ENTRIES);
  
  // Start from the most recent entry and go backward
  for (int i = 0; i < readCount; i++) {
    // Calculate the index to read, considering wraparound
    int readIndex = currentIndex - 1 - i;
    if (readIndex < 0) {
      readIndex += MAX_SENSOR_ENTRIES;
    }
    
    // Calculate the EEPROM address
    int address = SENSOR_DATA_START + readIndex * sizeof(int16_t);
    
    // Read the sensor value
    int16_t sensorValue;
    EEPROM.get(address, sensorValue);
    
    // Add to the history string
    historyData += String(sensorValue);
    if (i < readCount - 1) {
      historyData += ",";  // Separate values with commas
    }
  }
  
  return historyData;
}

void sendHistoryData(int count) {
  String historyData = readSensorHistory(count);
  if (historyData.length() == 0) {
    Serial.println("No history data available");
    return;
  }
  pHistoryCharacteristic->setValue(historyData.c_str());
  pHistoryCharacteristic->notify();
  Serial.println("Sent history data: " + historyData);
}


bool isAdvertising = false;
int sensorValue = 0;

void loop() {
  // If disconnected and not advertising, start advertising
  if (!deviceConnected && !isAdvertising) {
    delay(500);
    BLEDevice::startAdvertising();
    isAdvertising = true;
    Serial.println("Started advertising");
  }
  
  // main logic loop
  if (millis() - savedTime > 500) {
    digitalWrite(SENSOR_POWER_PIN, HIGH);
    delay(500); //(old code had 1000ms)
    sensorValue = analogRead(SENSOR_ANALOG_PIN);
    digitalWrite(SENSOR_POWER_PIN, LOW);
    Serial.print("Sensor value: ");
    Serial.println(sensorValue);

    // check for the AUTO mode
    if (currentMode == "AUTO") {
      Serial.print("Max level: ");
      Serial.println(currentMaxLevel);
      Serial.print("Min level: ");
      Serial.println(currentMinLevel);
            
      // Water level logic with both max and min levels
      if (sensorValue < currentMinLevel) {
        Serial.println("Water level is above minimum threshold. Turning PUMP ON.");
        // PUMP ON
        digitalWrite(PUMP_PIN, HIGH);
      } else if (sensorValue > currentMaxLevel) {
        // PUMP OFF
        Serial.println("Water level is below maximum threshold. Turning PUMP OFF.");
        digitalWrite(PUMP_PIN, LOW);
      }
    }
    
    savedTime = millis();
  }

  // save data each hour as history data
  if (millis() - lastSaveTime >= saveInterval) {
    saveDataFromSensor(sensorValue);
    Serial.print("Saved sensor value: ");
    Serial.println(sensorValue);
    lastSaveTime = millis();
  }

  // connect and send sensor value
  if (deviceConnected) {
    isAdvertising = false;
    pSensorCharacteristic->setValue(String(sensorValue).c_str());
    pSensorCharacteristic->notify();
    Serial.println("Sent notification: " + String(sensorValue));
    delay(500);
  }
  
  // Handle connection state changes
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    BLEDevice::startAdvertising(); 
    isAdvertising = true;
    Serial.println("Start advertising");
    oldDeviceConnected = deviceConnected;
  }
  
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }
}