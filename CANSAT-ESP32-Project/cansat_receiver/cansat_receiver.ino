/*
  CanSat base station (receiver) - ESP32

  Receives LoRa packets from the CanSat and prints them to the Arduino IDE
  Serial Monitor in real time, with signal strength and lost-packet count.

  Board:   "ESP32 Dev Module"
  Library: LoRa (Sandeep Mistry)
  Serial Monitor: 115200 baud

  Set PRETTY_OUTPUT to false to get plain CSV instead (paste into Excel, or
  copy from the Serial Monitor into a .csv file after the flight).
*/

#include <SPI.h>
#include <LoRa.h>

#define PRETTY_OUTPUT   true

// ---------------- Radio settings (MUST match the transmitter) ----------------
#define LORA_FREQ       433E6
#define LORA_SF         7
#define LORA_BW         125E3
#define LORA_CR         5
#define LORA_SYNC_WORD  0xF3

// ---------------- Pins (same LoRa wiring as the CanSat) ----------------
/*#define LORA_SCK    18
#define LORA_MISO   19
#define LORA_MOSI   23
#define LORA_CS     5
#define LORA_RST    14
#define LORA_DIO0   26
#define LED_PIN     2*/

// Packet: CS,<id>,ms,utc,temp_c,pressure_hpa,alt_m,lat,lon,gps_alt_m,sats,ax,ay,az,gx,gy,gz
enum { F_TAG, F_ID, F_MS, F_UTC, F_TEMP, F_PRESS, F_ALT, F_LAT, F_LON, F_GPSALT,
       F_SATS, F_AX, F_AY, F_AZ, F_GX, F_GY, F_GZ, F_COUNT };

const uint32_t SIGNAL_LOST_MS = 5000;

long lastId = -1;
uint32_t received = 0, lost = 0;
uint32_t lastPacketMs = 0;
bool lostWarned = false;

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(LED_PIN, OUTPUT);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setSPI(SPI);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("LoRa module NOT FOUND - check wiring");
    while (true) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(200); }
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();

  Serial.println("\n=== CanSat base station: listening ===");
  if (!PRETTY_OUTPUT)
    Serial.println("id,ms,utc,temp_c,pressure_hpa,alt_m,lat,lon,gps_alt_m,sats,"
                   "ax_ms2,ay_ms2,az_ms2,gx_dps,gy_dps,gz_dps,rssi_dbm,snr_db");
}

void printPretty(char *f[], int rssi, float snr) {
  Serial.printf("\n#%s  t=%.1fs  UTC %s   RSSI %d dBm  SNR %.1f dB  received %lu  lost %lu\n",
                f[F_ID], atol(f[F_MS]) / 1000.0, f[F_UTC], rssi, snr,
                (unsigned long)received, (unsigned long)lost);
  Serial.printf("  Temperature : %s C\n", f[F_TEMP]);
  Serial.printf("  Pressure    : %s hPa\n", f[F_PRESS]);
  Serial.printf("  Altitude    : %s m (above launch site)\n", f[F_ALT]);
  if (atoi(f[F_SATS]) > 0 && atof(f[F_LAT]) != 0.0)
    Serial.printf("  Latitude    : %s\n  Longitude   : %s\n  GPS altitude: %s m   Satellites: %s\n",
                  f[F_LAT], f[F_LON], f[F_GPSALT], f[F_SATS]);
  else
    Serial.printf("  GPS         : no fix yet (satellites: %s)\n", f[F_SATS]);
  Serial.printf("  Accel m/s2  : x %s  y %s  z %s\n", f[F_AX], f[F_AY], f[F_AZ]);
  Serial.printf("  Gyro deg/s  : x %s  y %s  z %s\n", f[F_GX], f[F_GY], f[F_GZ]);
}

void loop() {
  int size = LoRa.parsePacket();
  if (size) {
    char buf[256];
    int n = 0;
    while (LoRa.available() && n < (int)sizeof(buf) - 1) buf[n++] = (char)LoRa.read();
    while (LoRa.available()) LoRa.read();
    buf[n] = '\0';
    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();

    if (strncmp(buf, "CS,", 3) != 0) return;   // not our CanSat

    // Split into fields
    char *f[F_COUNT];
    int count = 0;
    for (char *tok = strtok(buf, ","); tok && count < F_COUNT; tok = strtok(NULL, ","))
      f[count++] = tok;
    if (count != F_COUNT) {
      Serial.printf("Malformed packet (%d fields)\n", count);
      return;
    }

    long id = atol(f[F_ID]);
    if (lastId >= 0 && id > lastId + 1) lost += id - lastId - 1;
    if (id < lastId) Serial.println("-- CanSat restarted --");
    lastId = id;
    received++;
    lastPacketMs = millis();
    lostWarned = false;
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));

    if (PRETTY_OUTPUT) {
      printPretty(f, rssi, snr);
    } else {
      for (int i = F_ID; i < F_COUNT; i++) { Serial.print(f[i]); Serial.print(','); }
      Serial.printf("%d,%.1f\n", rssi, snr);
    }
  }

  if (received && !lostWarned && millis() - lastPacketMs > SIGNAL_LOST_MS) {
    Serial.printf("!! No packet for %lu s - signal lost\n", (unsigned long)(SIGNAL_LOST_MS / 1000));
    lostWarned = true;
  }
}
