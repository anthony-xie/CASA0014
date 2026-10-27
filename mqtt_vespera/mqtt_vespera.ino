#include <SPI.h>
#include <WiFiNINA.h>
#include <PubSubClient.h>
#include "arduino_secrets.h" 
#include <utility/wifi_drv.h> // Library to drive RGB LED on MKR1010
#include "Wire.h"
#include "I2C.h"

#define MPU9250_IMU_ADDRESS 0x68
#define MPU9250_MAG_ADDRESS 0x0C

#define GYRO_FULL_SCALE_250_DPS  0x00
#define GYRO_FULL_SCALE_500_DPS  0x08
#define GYRO_FULL_SCALE_1000_DPS 0x10
#define GYRO_FULL_SCALE_2000_DPS 0x18

#define ACC_FULL_SCALE_2G  0x00
#define ACC_FULL_SCALE_4G  0x08
#define ACC_FULL_SCALE_8G  0x10
#define ACC_FULL_SCALE_16G 0x18

#define TEMPERATURE_OFFSET 21 // As defined in documentation

#define INTERVAL_MS_PRINT 1000

#define G 9.80665

struct gyroscope_raw {
  int16_t x, y, z;
} gyroscope;

struct accelerometer_raw {
  int16_t x, y, z;
} accelerometer;

struct magnetometer_raw {
  int16_t x, y, z;

  struct {
    int8_t x, y, z;
  } adjustment;
} magnetometer;

struct temperature_raw {
  int16_t value;
} temperature;

struct {
  struct {
    float x, y, z;
  } accelerometer, gyroscope, magnetometer;

  float temperature;
} normalized;

unsigned long lastPrintMillis = 0;

const char* ssid          = SECRET_SSID;
const char* password      = SECRET_PASS;
const char* mqtt_username = SECRET_MQTTUSER;
const char* mqtt_password = SECRET_MQTTPASS;
const char* mqtt_server   = "mqtt.cetools.org";
const int mqtt_port       = 1884;

// Create WiFi and MQTT client objects
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// Update lightId with your allocated user number (e.g. "0", "1", "2", etc.)
String lightId = "23";
String mqtt_topic = "student/CASA0014/luminaire/" + lightId;            
String clientId = ""; // Unique client ID based on MAC address

// Hemisphere Grid Geometry
const int NUM_COLUMNS = 12;      // 12 columns around the perimeter
const int PIXELS_PER_COL = 6;    // 6 vertical LEDs per column
const int num_leds = NUM_COLUMNS * PIXELS_PER_COL; // 72 LEDs total
const int payload_size = num_leds * 3;             // 3 bytes per LED (R, G, B)

// Array holding RGB values for all 72 LEDs to send in MQTT payload
byte RGBpayload[payload_size];

void setup() {
  while (!Serial){};
  Serial.begin(115200);
  Serial.println(F("Starting Vespera Testing..."));
  Wire.begin();
  
  I2CwriteByte(MPU9250_IMU_ADDRESS, 27, GYRO_FULL_SCALE_1000_DPS); // Configure gyroscope range
  I2CwriteByte(MPU9250_IMU_ADDRESS, 28, ACC_FULL_SCALE_2G);        // Configure accelerometer range


  I2CwriteByte(MPU9250_IMU_ADDRESS, 55, 0x02); // Set by pass mode for the magnetometers
  I2CwriteByte(MPU9250_IMU_ADDRESS, 56, 0x01); // Enable interrupt pin for raw data
  

  setMagnetometerAdjustmentValues();
  

  //Start magnetometer
  I2CwriteByte(MPU9250_MAG_ADDRESS, 0x0A, 0x12); // Request continuous magnetometer measurements in 16 bits (mode 1)

  
  // Status LEDs on MKR1010 NINA module
  WiFiDrv::pinMode(25, OUTPUT); // R
  WiFiDrv::pinMode(26, OUTPUT); // G
  WiFiDrv::pinMode(27, OUTPUT); // B
  LedRed();

  // Generate a unique client ID using MAC address
  byte mac[6];
  WiFi.macAddress(mac);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X", mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
  clientId = "MKR1010_Sender_" + String(macStr);

  Serial.print(F("Device Client ID: "));
  Serial.println(clientId);
  Serial.print(F("Publishing to topic: "));
  Serial.println(mqtt_topic);

  // Connect to WiFi & MQTT
  startWifi();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(30);


  Serial.println(F("Setup complete!"));
}

void loop() {
  // Ensure network connectivity
  if (WiFi.status() != WL_CONNECTED) {
    startWifi();
  }
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();
  unsigned long currentMillis = millis();

  if (isImuReady()) {
    readRawImu();
    normalize(gyroscope);
    normalize(accelerometer);
    normalize(temperature);
  }

  if (currentMillis - lastPrintMillis > INTERVAL_MS_PRINT) {
    int x = normalized.accelerometer.x;
    int y = normalized.accelerometer.y;
    int z = normalized.accelerometer.z;
    Serial.println(x);
    Serial.println(y);
    Serial.println(z);
    send_colour_to_all(x*20, y*20, z*5);

    Serial.println();

    lastPrintMillis = currentMillis;
  }

  // Pause at the end of the cycle
  delay(1000);
}

// Sets all 72 LEDs to the given (r, g, b) color and publishes to MQTT
void send_colour_to_all(int r, int g, int b) {
  if (mqttClient.connected()) {
    for (int pixel = 0; pixel < num_leds; pixel++) {
      RGBpayload[pixel * 3 + 0] = (byte)constrain(r, 0, 255);
      RGBpayload[pixel * 3 + 1] = (byte)constrain(g, 0, 255);
      RGBpayload[pixel * 3 + 2] = (byte)constrain(b, 0, 255);
    }
    mqttClient.publish(mqtt_topic.c_str(), RGBpayload, payload_size);
  } else {
    Serial.println(F("MQTT client not connected, cannot publish."));
  }
}

// Turns all 72 LEDs off (black)
void send_all_off() {
  send_colour_to_all(0, 0, 0);
}