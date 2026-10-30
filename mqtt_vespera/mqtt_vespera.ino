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

//Colours that need to be caught (ROYGBIV in reverse as it is faster to remove the end of a string)
String rainbow = "VIBGYO";

// red dot (current dot) always starts in the same square
int curr = 3;
// first target colour is orange as it comes after red
int target = -1;
char targetColour = 'O';
// turn true when all the colours have been caught
bool gameComplete = false;

//create an array representation of the LED lights
bool excluded[num_leds];

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

  //RNG Seed
  randomSeed(42);

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
  
  target = chooseLED(curr, excluded);

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

  if (gameComplete) {
    rainbowCycle();
    return;
  }

  if (isImuReady()) {
    readRawImu();
    normalize(gyroscope);
    normalize(accelerometer);
    normalize(temperature);
  }
  
  if (currentMillis - lastPrintMillis > INTERVAL_MS_PRINT) {
    int gx = normalized.gyroscope.x;
    int gy = normalized.gyroscope.y;
    int gz = normalized.gyroscope.z;
    
    Serial.println(gx);
    Serial.println(gy);
    Serial.println(gz);
    Serial.println();
    move(gx, gy);
    if(curr == target){
      captured();
    }

    if(!gameComplete){
      setColour(curr, 'R');
      setColour(target, targetColour);
    }

    lastPrintMillis = currentMillis;
  }

  // Pause at the end of the cycle
  delay(1000);
}

//Movement Logic
void move(int gx, int gy){
  int absGx = abs(gx);
  int absGy = abs(gy);
  if (max(absGx, absGy) < 30) {
    return;
  }

  //ignore gz because it only affects rotation around the z-axis (ie when you turn the sensor on a flat surface)
  int col = curr / PIXELS_PER_COL;
  int row = curr % PIXELS_PER_COL;

  //set the previous LED to blank
  RGBpayload[curr * 3 + 0] = (byte)0;
  RGBpayload[curr * 3 + 1] = (byte)0;
  RGBpayload[curr * 3 + 2] = (byte)0;
  mqttClient.publish(mqtt_topic.c_str(), RGBpayload, payload_size);

  // Move along the axis with the larger absolute gyro reading.
  if (absGx >= absGy) {
    if (gx > 0) {
      row = (row - 1 + PIXELS_PER_COL) % PIXELS_PER_COL;
    } else {
      row = (row + 1) % PIXELS_PER_COL;
    }
  } else {
    if (gy > 0) {
      col = (col - 1 + NUM_COLUMNS) % NUM_COLUMNS;
    } else {
      col = (col + 1) % NUM_COLUMNS;
    }
  }

  curr = col * PIXELS_PER_COL + row;
}


// Mark LEDs that are too close to the current point (around a 5x5 box)
// and then choose a random LED that is available
int chooseLED(int curr, bool excluded[num_leds]) {
  for (int i = 0; i < num_leds; i++) {
    excluded[i] = false;
  }
  
  int col = curr / PIXELS_PER_COL;
  int row = curr % PIXELS_PER_COL;

  //iterate from curr col - 2 to curr col + 2
  for (int dc = -2; dc <= 2; dc++) {
    int c = (col + dc + NUM_COLUMNS) % NUM_COLUMNS;
    //iterate from curr row - 2 to curr row + 2
    for (int dr = -2; dr <= 2; dr++) {
      int r = (row + dr + PIXELS_PER_COL) % PIXELS_PER_COL;
      excluded[c * PIXELS_PER_COL + r] = true;
    }
  }
  
  int available[num_leds];
  int count = 0;

  //iterate through the LEDs and exclude the LEDs within the 5x5 box
  for (int i = 0; i < num_leds; i++) {
    if (!excluded[i]) {
      available[count++] = i;
    }
  }

  return available[random(count)];
}

void setColour(int pixel, char colour){
  if (mqttClient.connected()){
    switch(colour){
      //red
      case 'R':
        set_pixel(pixel, 255, 0, 0);
        break;
      
      //orange
      case 'O':
        set_pixel(pixel, 255, 165, 0);
        break;

      //yellow
      case 'Y':
        set_pixel(pixel, 255, 255, 0);
        break;
      
      //green
      case 'G':
        set_pixel(pixel, 0, 255, 0);
        break;
      
      //blue
      case 'B':
        set_pixel(pixel, 0, 0, 255);
        break;
      
      //indigo
      case 'I':
        set_pixel(pixel, 75, 0, 130);
        break;
      
      //violet
      case 'V':
        set_pixel(pixel, 238, 130, 238);
        break;

      default:
        return;
    }

    mqttClient.publish(mqtt_topic.c_str(), RGBpayload, payload_size);
  }
}

//Collision Logic (What happens when the red dot "catches" the target dot)
void captured(){
  rainbow.remove(rainbow.length() - 1);
  if (rainbow.length() == 0) {
    gameComplete = true;
    return;
  }

  targetColour = rainbow.charAt(rainbow.length() - 1);
  target = chooseLED(curr, excluded);
}


// Sets the color of all 6 vertical pixels in a specific column (0 to 11)
void set_column_color(int col, int r, int g, int b) {
  if (col < 0 || col >= NUM_COLUMNS) return;

  int startPixel = col * PIXELS_PER_COL; // e.g. Col 0 -> 0..5, Col 1 -> 6..11
  for (int row = 0; row < PIXELS_PER_COL; row++) {
    set_pixel(startPixel + row, r, g, b);
  }
}

// Sets the RGB color of a single pixel (0 to 71) in the local buffer
void set_pixel(int pixel, int r, int g, int b) {
  if (pixel < 0 || pixel >= num_leds) return;

  RGBpayload[pixel * 3 + 0] = (byte)constrain(r, 0, 255);
  RGBpayload[pixel * 3 + 1] = (byte)constrain(g, 0, 255);
  RGBpayload[pixel * 3 + 2] = (byte)constrain(b, 0, 255);
}

// Columns rotate in 
void rainbowCycle() {
  static const byte colors[][3] = {
    {255, 0, 0},     // Red
    {255, 165, 0},   // Orange
    {255, 255, 0},   // Yellow
    {0, 255, 0},     // Green
    {0, 0, 255},     // Blue
    {75, 0, 130},    // Indigo
    {238, 130, 238}  // Violet
  };
  static const int colorCount = sizeof(colors) / sizeof(colors[0]);
  static int colorOffset = 0;
  static unsigned long lastChange = 0;
  static bool firstUpdate = true;

  unsigned long now = millis();
  if (!firstUpdate && now - lastChange < 500) {
    return;
  }

  firstUpdate = false;
  lastChange = now;

  for (int col = 0; col < NUM_COLUMNS; col++) {
    int colorIndex = (col + colorOffset) % colorCount;
    set_column_color(
      col,
      colors[colorIndex][0],
      colors[colorIndex][1],
      colors[colorIndex][2]
    );
  }

  if (mqttClient.connected()) {
    mqttClient.publish(mqtt_topic.c_str(), RGBpayload, payload_size);
  }

  colorOffset = (colorOffset + 1) % colorCount;
}

// Turns all 72 LEDs off (black)
void send_all_off() {
  for (int pixel = 0; pixel < num_leds; pixel++) {
    set_pixel(pixel, 0, 0, 0);
  }

  if (mqttClient.connected()) {
    mqttClient.publish(mqtt_topic.c_str(), RGBpayload, payload_size);
  }
}