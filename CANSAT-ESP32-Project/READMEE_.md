# CanSat build guide

Two ESP32 boards:

- **CanSat (transmitter)** `cansat_transmitter/cansat_transmitter.ino` reads the BMP280, MPU6050 and GPS, logs every reading to the microSD card at 5 Hz, and sends one reading per second over LoRa.
- **Base station (receiver)** `cansat_receiver/cansat_receiver.ino` receives the LoRa packets and prints them live in the Arduino IDE Serial Monitor (115200 baud), with signal strength and a lost-packet counter.

Both sketches compile without errors for **ESP32 Dev Module** (esp32 core 3.0.7). They have not been run on real hardware yet, so do the bench test at the end before flying.

## Parts assumed

Tell me if any of yours differ, because pins and code change with the part.

| Part | Assumed model | Notes |
| --- | --- | --- |
| Microcontroller x2 | ESP32 DevKit V1 (ESP32-WROOM-32) | one in the CanSat, one at the base station |
| Pressure/temperature | BMP280 breakout (I2C) | a BME280 also works with this code |
| IMU | MPU6050 (GY-521) |  |
| GPS | u-blox NEO-6M (GY-NEO6MV2) with its patch antenna | needed for latitude/longitude |
| Radio x2 | SX1278 LoRa, e.g. Ai-Thinker Ra-02, 433 MHz | **3.3 V only.** Use the band that is legal where you fly (see below) |
| Antennas x2 | 433 MHz antenna (IPEX/SMA) or a 17.3 cm straight wire | never transmit without an antenna, it can damage the module |
| Storage | microSD SPI module + microSD card ≤ 32 GB formatted FAT32 |  |
| Battery | 1S LiPo 3.7 V, 500 to 1000 mAh |  |
| Power | MT3608 (or similar) boost converter set to 5.0 V, plus a slide switch |  |
| Recovery | parachute (sized below), nylon shroud lines, swivel |  |

Typical current draw is about 150 to 250 mA (radio bursts to about 120 mA), so a 1000 mAh LiPo gives roughly 3 hours.

### Radio band
433 MHz is licence-free in most of Asia and Europe at low power, 868 MHz is the usual European band, and 915 MHz is used in the Americas and Australia. Buy the module for your band and set `LORA_FREQ` to the same value in **both** sketches. Many regions limit power and duty cycle: each packet is about 120 bytes and takes about 0.2 s on air at the default settings (about 20% duty cycle at 1 packet per second). If your rules allow 10%, set `TX_INTERVAL_MS` to `2000`; if they limit power to 10 mW, set `LORA_TX_POWER` to `10`. Competitions often assign you a frequency; use theirs.

## Wiring: CanSat (transmitter)

| Module pin | ESP32 pin | Notes |
| --- | --- | --- |
| **BMP280** VCC / GND | 3V3 / GND |  |
| BMP280 SDA / SCL | GPIO 21 / GPIO 22 | shared I2C bus |
| BMP280 SDO | GND (address 0x76) | the code also tries 0x77 |
| BMP280 CSB | 3V3 | selects I2C mode (many boards already do this) |
| **MPU6050** VCC / GND | 3V3 / GND |  |
| MPU6050 SDA / SCL | GPIO 21 / GPIO 22 | same bus as BMP280 |
| **GPS** VCC / GND | 3V3 (or 5V) / GND | GY-NEO6MV2 has its own regulator |
| GPS TX | GPIO 16 (RX2) | crossed: GPS TX goes to ESP32 RX |
| GPS RX | GPIO 17 (TX2) |  |
| **LoRa Ra-02** 3.3V / GND | 3V3 / GND | **not 5 V** |
| LoRa SCK / MISO / MOSI | GPIO 18 / 19 / 23 | VSPI bus |
| LoRa NSS | GPIO 5 |  |
| LoRa RST | GPIO 14 |  |
| LoRa DIO0 | GPIO 26 |  |
| **microSD** VCC / GND | 5V (VIN) / GND | modules with a regulator need 5 V; plain ones without a regulator go on 3V3 |
| microSD SCK / MISO / MOSI | GPIO 25 / 27 / 32 | separate bus (HSPI) so the SD card can't disturb the radio |
| microSD CS | GPIO 33 |  |
| **Power** LiPo + | switch, then boost converter IN+ |  |
| Boost OUT+ (5.0 V) / OUT- | ESP32 VIN / GND | set the boost to 5.0 V with a multimeter **before** connecting |

All GND pins must be connected together. Keep I2C wires short (under 15 cm).

## Wiring: base station (receiver)

Only the LoRa module, wired exactly like the CanSat:

| LoRa pin          | ESP32 pin |
| 3.3V / GND        | 3V3 / GND |
| SCK / MISO / MOSI | GPIO 18 / 19 / 23 |
| NSS               | GPIO 5 |
| RST               | GPIO 14 |
| DIO0              | GPIO 26 |

Power it from the laptop's USB. For more range, use a bigger antenna (a Yagi or a 433 MHz whip on a tripod) and point it at the CanSat.

## Arduino IDE setup

1. **File > Preferences > Additional boards manager URLs**: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. **Tools > Board > Boards Manager**: install **esp32 by Espressif Systems**.
3. **Tools > Board**: select **ESP32 Dev Module**.
4. **Sketch > Include Library > Manage Libraries**, install:
- **LoRa** by Sandeep Mistry
- **TinyGPSPlus** by Mikal Hart
- **Adafruit BMP280 Library**
- **Adafruit MPU6050**
- **Adafruit Unified Sensor** and **Adafruit BusIO** (the IDE offers these as dependencies; click "Install all")
5. Open each `.ino` (keep it inside its folder of the same name), select the board's COM port, and upload.
6. Open **Tools > Serial Monitor** and set it to **115200 baud**.

## What you will see

Base station Serial Monitor (default `PRETTY_OUTPUT true`):

```
#42  t=43.2s  UTC 10:22:31   RSSI -87 dBm  SNR 9.5 dB  received 42  lost 0
  Temperature : 27.41 C
  Pressure    : 1004.86 hPa
  Altitude    : 152.3 m (above launch site)
  Latitude    : 12.971234
  Longitude   : 77.594321
  GPS altitude: 1052.4 m   Satellites: 8
  Accel m/s2  : x 0.12  y 0.03  z 9.79
  Gyro deg/s  : x 1.2  y -0.3  z 0.5
```

Set `PRETTY_OUTPUT false` in the receiver sketch to print one CSV line per packet instead, which you can paste into Excel or use with **Tools > Serial Plotter**.

The SD card gets a new file each power-on (`LOG000.CSV`, `LOG001.CSV`, ...) with 5 rows per second:

```
ms,utc,temp_c,pressure_hpa,alt_m,lat,lon,gps_alt_m,sats,ax_ms2,ay_ms2,az_ms2,gx_dps,gy_dps,gz_dps
```

`alt_m` is altitude above the place where the CanSat was switched on (the code measures ground pressure at power-on), so **switch it on at the launch site**. Latitude/longitude show `0.000000` until the GPS has a fix. The file is saved once per second, so a sudden power cut loses at most one second.

## Parachute sizing

The parachute's job is to bring the CanSat down slowly enough to survive and to stay readable, but not so slowly that it drifts far away. The required area is:

**A = 2·m·g / (ρ · Cd · v²)**, then **diameter D = √(4A / π)**

where m is the mass in kg, g = 9.81 m/s², ρ = 1.225 kg/m³ (air at sea level), Cd ≈ 0.8 for a flat round or hexagonal canopy, and v is the target descent speed.

| CanSat mass | 5 m/s (gentle, drifts far) | 8 m/s | 10 m/s | 11 m/s |
| --- | --- | --- | --- | --- |
| 300 g | 55 cm | 35 cm | 28 cm | 25 cm |
| 350 g | 60 cm | 37 cm | 30 cm | 27 cm |
| 500 g | 71 cm | 45 cm | 36 cm | 32 cm |

Many CanSat competitions require a descent speed of about 8 to 11 m/s; check your rules, then weigh your finished CanSat and pick from the table.

Build tips:

- Cut a hexagon from ripstop nylon; D in the table is the distance across the corners.
- Cut a **spill hole** in the centre about 10 to 15% of D. It stops the canopy swinging and makes the readings smoother.
- Use 6 shroud lines, each about 1 to 1.5 × D long, joined to a fishing **swivel** so the CanSat can spin without twisting the lines.
- Attach the swivel to a strong point on the CanSat frame (not to a PCB).
- Drop-test from a height (a building or drone) and time the fall over a known distance. Your logged `alt_m` also gives the real descent speed: change in altitude divided by change in time.

## Bench test before flight

1. Power the receiver from your laptop and open its Serial Monitor.
2. Power the CanSat from USB on a second port (or the battery) and open its Serial Monitor too. It prints `OK` / `NOT FOUND` for each sensor at start-up. The on-board LED blinks 3 times quickly if everything is found, slowly if something is missing.
3. Take the CanSat outside with clear sky and wait for a GPS fix (1 to 5 minutes the first time).
4. Walk away with it to check range, watching RSSI on the base station. Packets usually stay readable down to about -120 dBm at these settings.
5. Switch off, put the SD card in a computer, and open the newest `LOGxxx.CSV`.

## Troubleshooting

- **BMP280 NOT FOUND**: check SDA/SCL are not swapped and CSB is tied high. Run an I2C scanner sketch: you should see 0x76 (or 0x77) and 0x68.
- **LoRa NOT FOUND**: almost always a wiring issue on NSS/RST or SPI pins, or the module is not getting 3.3 V.
- **SD NOT FOUND**: the card must be FAT32 and 32 GB or smaller; try 5 V on the module's VCC.
- **No packets received**: `LORA_FREQ`, `LORA_SF`, `LORA_BW` and `LORA_SYNC_WORD` must be identical in both sketches; both antennas attached.
- **GPS shows satellites 0**: it has to see the sky (it won't work indoors); the GPS LED blinks once per second when it has a fix.
- **Upload fails with "Failed to connect"**: hold the BOOT button while the IDE shows "Connecting...".
