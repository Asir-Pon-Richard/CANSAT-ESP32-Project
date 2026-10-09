/*
  CanSat flight unit (transmitter) - ESP32

  Reads:   BMP280 (temperature, pressure, altitude above launch site)
           MPU6050 IMU (acceleration, rotation rate)
           NEO-6M GPS (latitude, longitude, GPS altitude, satellites, UTC time)
  Logs:    every sample to a CSV file on the microSD card (5 Hz)
  Sends:   one sample per second over LoRa to the base station (1 Hz)

  Board:   "ESP32 Dev Module" (Arduino IDE, esp32 core by Espressif)
  Libraries (Library Manager):
           LoRa (Sandeep Mistry), TinyGPSPlus (Mikal Hart),
           Adafruit BMP280 Library, Adafruit MPU6050,
           Adafruit Unified Sensor, Adafruit BusIO
  Serial Monitor: 115200 baud (debug output while the CanSat is on USB)

  Wiring: see README.md
*/

#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <LoRa.h>
#include <TinyGPSPlus.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_MPU6050.h>

// ---------------- Radio settings (MUST match the receiver) ----------------
// Use the band that is legal where you fly: 433E6 (Asia/EU 433 modules),
// 868E6 (Europe), 915E6 (Americas/Australia). The module must match too.
#define LORA_FREQ       433E6
#define LORA_SF         7        // 7 = fastest, 9-10 = longer range, slower
#define LORA_BW         125E3
#define LORA_CR         5        // coding rate 4/5
#define LORA_SYNC_WORD  0xF3     // private network id, keeps other LoRa traffic out
#define LORA_TX_POWER   17       // dBm (2..20). Check your local legal limit.

// ---------------- Timing ----------------
const uint32_t LOG_INTERVAL_MS = 200;    // SD logging: 5 samples per second
const uint32_t TX_INTERVAL_MS  = 1000;   // LoRa: 1 packet per second
const uint32_t FLUSH_INTERVAL_MS = 1000; // SD flush: lose at most 1 s on power cut

// ---------------- Pins (ESP32 DevKit V1, 30/38 pin) ----------------
// I2C bus: BMP280 + MPU6050
#define I2C_SDA     21
#define I2C_SCL     22
// GPS on UART2
#define GPS_RX_PIN  16   // ESP32 RX2  <- GPS TX
#define GPS_TX_PIN  17   // ESP32 TX2  -> GPS RX
#define GPS_BAUD    9600
// LoRa SX1278 / Ra-02 on VSPI
#define LORA_SCK    18
#define LORA_MISO   19
#define LORA_MOSI   23
#define LORA_CS     5
#define LORA_RST    14
#define LORA_DIO0   26
// microSD on its own SPI bus (HSPI) so it can't disturb the radio
#define SD_SCK      25
#define SD_MISO     27
#define SD_MOSI     32
#define SD_CS       33
// On-board LED: blinks on every LoRa packet
#define LED_PIN     2

// ---------------- Objects ----------------
Adafruit_BMP280 bmp;              // I2C
Adafruit_MPU6050 mpu;
TinyGPSPlus gps;
HardwareSerial GPSSerial(2);
SPIClass sdSPI(HSPI);
File logFile;

bool bmpOk = false, mpuOk = false, sdOk = false, loraOk = false;
char logName[16] = "";
float groundPressureHpa = 1013.25;  // measured at power-on, altitude is relative to it
uint32_t packetId = 0;
uint32_t lastLogMs = 0, lastTxMs = 0, lastFlushMs = 0, lastSdRetryMs = 0;

const char CSV_HEADER[] =
  "ms,utc,temp_c,pressure_hpa,alt_m,lat,lon,gps_alt_m,sats,"
  "ax_ms2,ay_ms2,az_ms2,gx_dps,gy_dps,gz_dps";

struct Sample {
  uint32_t ms;
  char utc[12];                // hh:mm:ss
  float tempC, pressHpa, altM;
  double lat, lon;
  float gpsAltM;
  int sats;
  float ax, ay, az;            // m/s^2
  float gx, gy, gz;            // deg/s
};

// ---------------------------------------------------------------------------

void feedGps() {
  while (GPSSerial.available()) gps.encode(GPSSerial.read());
}

bool initBmp() {
  // Most breakout boards use 0x76, some 0x77. Many "BMP280" boards are really
  // BME280 (chip id 0x60); temperature/pressure registers are identical.
  const uint8_t addrs[] = {0x76, 0x77};
  const uint8_t ids[] = {BMP280_CHIPID, 0x60};
  for (uint8_t a : addrs)
    for (uint8_t id : ids)
      if (bmp.begin(a, id)) {
        bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                        Adafruit_BMP280::SAMPLING_X2,    // temperature
                        Adafruit_BMP280::SAMPLING_X16,   // pressure
                        Adafruit_BMP280::FILTER_X4,
                        Adafruit_BMP280::STANDBY_MS_1);
        return true;
      }
  return false;
}

void calibrateGroundPressure() {
  // Average 20 readings so "alt_m" reads 0 at the launch site.
  delay(100);
  float sum = 0;
  for (int i = 0; i < 20; i++) {
    sum += bmp.readPressure() / 100.0f;
    delay(50);
  }
  groundPressureHpa = sum / 20.0f;
}

bool initMpu() {
  if (!mpu.begin(0x68, &Wire) && !mpu.begin(0x69, &Wire)) return false;
  mpu.setAccelerometerRange(MPU6050_RANGE_16_G);   // parachute opening shock
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  return true;
}

bool openLogFile() {
  // New file every power-on: LOG000.CSV, LOG001.CSV, ...
  for (int i = 0; i < 1000; i++) {
    snprintf(logName, sizeof(logName), "/LOG%03d.CSV", i);
    if (!SD.exists(logName)) break;
  }
  logFile = SD.open(logName, FILE_WRITE);
  if (!logFile) return false;
  logFile.println(CSV_HEADER);
  logFile.flush();
  return true;
}

bool initSd() {
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, 4000000)) return false;
  return openLogFile();
}

bool initLora() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setSPI(SPI);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) return false;
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  LoRa.setTxPower(LORA_TX_POWER);
  return true;
}

void readSample(Sample &s) {
  s.ms = millis();

  if (bmpOk) {
    s.tempC = bmp.readTemperature();
    s.pressHpa = bmp.readPressure() / 100.0f;
    s.altM = bmp.readAltitude(groundPressureHpa);
  } else {
    s.tempC = s.pressHpa = s.altM = NAN;
  }

  if (mpuOk) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
    s.ax = a.acceleration.x;  s.ay = a.acceleration.y;  s.az = a.acceleration.z;
    s.gx = g.gyro.x * RAD_TO_DEG;  s.gy = g.gyro.y * RAD_TO_DEG;  s.gz = g.gyro.z * RAD_TO_DEG;
  } else {
    s.ax = s.ay = s.az = s.gx = s.gy = s.gz = NAN;
  }

  // Treat a fix older than 3 s as lost
  bool fix = gps.location.isValid() && gps.location.age() < 3000;
  s.lat = fix ? gps.location.lat() : 0.0;
  s.lon = fix ? gps.location.lng() : 0.0;
  s.gpsAltM = (fix && gps.altitude.isValid()) ? gps.altitude.meters() : NAN;
  s.sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  if (gps.time.isValid())
    snprintf(s.utc, sizeof(s.utc), "%02d:%02d:%02d",
             gps.time.hour(), gps.time.minute(), gps.time.second());
  else
    strcpy(s.utc, "--:--:--");
}

// One CSV line, same columns as CSV_HEADER
int formatSample(const Sample &s, char *buf, size_t len) {
  return snprintf(buf, len,
    "%lu,%s,%.2f,%.2f,%.1f,%.6f,%.6f,%.1f,%d,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f",
    (unsigned long)s.ms, s.utc, s.tempC, s.pressHpa, s.altM,
    s.lat, s.lon, s.gpsAltM, s.sats,
    s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
}

void logToSd(const char *line) {
  if (!sdOk) {
    // Card missing or knocked loose: try to remount every 5 s
    if (millis() - lastSdRetryMs > 5000) {
      lastSdRetryMs = millis();
      SD.end();
      sdOk = initSd();
      if (sdOk) Serial.printf("SD remounted, logging to %s\n", logName);
    }
    return;
  }
  if (logFile.println(line) == 0) {
    Serial.println("SD write failed");
    logFile.close();
    sdOk = false;
    return;
  }
  if (millis() - lastFlushMs >= FLUSH_INTERVAL_MS) {
    lastFlushMs = millis();
    logFile.flush();
  }
}

void sendLora(const char *line) {
  if (!loraOk) return;
  // Non-blocking send; skip this slot if the previous packet is still on air
  if (!LoRa.beginPacket()) return;
  LoRa.print("CS,");
  LoRa.print(packetId++);
  LoRa.print(',');
  LoRa.print(line);
  LoRa.endPacket(true);
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(LED_PIN, OUTPUT);
  Serial.println("\n=== CanSat transmitter ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  GPSSerial.setRxBufferSize(1024);
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  bmpOk = initBmp();
  if (bmpOk) calibrateGroundPressure();
  mpuOk = initMpu();
  sdOk = initSd();
  loraOk = initLora();

  Serial.printf("BMP280 : %s", bmpOk ? "OK" : "NOT FOUND");
  if (bmpOk) Serial.printf("  (ground pressure %.2f hPa)", groundPressureHpa);
  Serial.printf("\nMPU6050: %s\n", mpuOk ? "OK" : "NOT FOUND");
  Serial.printf("SD card: %s%s\n", sdOk ? "OK, logging to " : "NOT FOUND", sdOk ? logName : "");
  Serial.printf("LoRa   : %s\n", loraOk ? "OK" : "NOT FOUND");
  Serial.println("GPS    : waiting for fix (needs open sky, can take 1-5 min cold start)");
  Serial.println(CSV_HEADER);

  // Fast blink 3x = everything up, long on = something missing
  bool allOk = bmpOk && mpuOk && sdOk && loraOk;
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH); delay(allOk ? 100 : 600);
    digitalWrite(LED_PIN, LOW);  delay(100);
  }
}

void loop() {
  feedGps();

  uint32_t now = millis();
  if (now - lastLogMs < LOG_INTERVAL_MS) return;
  lastLogMs = now;

  Sample s;
  readSample(s);
  char line[200];
  formatSample(s, line, sizeof(line));

  logToSd(line);

  if (now - lastTxMs >= TX_INTERVAL_MS) {
    lastTxMs = now;
    sendLora(line);
    Serial.println(line);   // 1 line per second on USB for bench testing
  }
}
