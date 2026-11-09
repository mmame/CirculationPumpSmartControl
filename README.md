# CirculationPumpSmartControl

Self-learning hot water circulation pump control for an ESP32.

The controller watches the temperature of a pipe with a DS18B20 sensor. When a temperature
drop shows that hot water was used, it remembers the weekday and the 15 minute time slot.
From these observations it builds a weekly usage pattern and switches the circulation pump on
only in the time slots in which hot water is usually needed.

## Hardware

| Part | Pin |
|---|---|
| Pump relay | GPIO 27 |
| DS18B20 temperature sensor (OneWire) | GPIO 32 |
| PCF8563 RTC (I2C, optional) | SDA GPIO 21, SCL GPIO 22 |

Board: `esp32dev`. The schematic and PCB are in `EasyEDA/`.
The RTC is optional: without it the time is taken from NTP (`pool.ntp.org`), the time zone is
looked up online.

## How it works

* Every 30 s a measurement cycle runs: 5 s of measuring (average of all readings), 25 s pause.
* If the average is at least `threshold` (default 2.0 °C) below the average of the previous cycle,
  a **hot water usage** is detected.
* The week is divided into 7 x 96 slots of 15 minutes. A detected usage raises the counter of
  the current slot by `increaseOnUsage`; a slot that ends without usage lowers its counter by 1.
* The pump runs in every slot whose counter is above 0. The **Enable Pump** button (web UI,
  Home Assistant) raises the counter of the current slot by one and so starts the pump right away.
* The usage grid can be viewed, edited and reset in the web UI.

## Web UI

Open `http://pump.local/` (or the IP address).

| Page | Purpose |
|---|---|
| `/` | status, statistics, Enable Pump, restart |
| `/grid` | edit the learned usage pattern |
| `/config` | thresholds and timing |
| `/wificonfig` | WiFi credentials (without credentials the device opens the access point `CirculationPumpSmartControl`) |
| `/timeconfig` | set the RTC |
| `/mqttconfig` | MQTT / Home Assistant |
| `/update` | firmware upload |

JSON endpoints: `GET /getstatus`, `GET /getstats`, `POST /enablepump`.

## Statistics (rolling 7 days)

All values are measured on the device and survive reboots (ring buffer of 7 x 96 slots in flash).
Values that cannot be determined reliably are **not shown** instead of being estimated.

| Value | Meaning | Shown when |
|---|---|---|
| Pump running | pump run time / covered time | at least 1 h of data |
| Pump run time | hours the relay was on | always |
| Usage events | slots with a detected usage | always |
| Pump already on at usage | share of usages that found the pump already running | at least 10 usages |
| Run time without usage | share of the run time in slots without a usage | at least 1 h of run time |
| Suspect detections | detections within 3 min after the pump switched on/off, possibly caused by the pump itself and excluded from the two quotas above | always |
| Scheduled share | share of the 672 slots in which the pattern would run the pump | always |

## Home Assistant (MQTT)

Configure the broker under `/mqttconfig` (host, port, user, password). The device then publishes
Home Assistant MQTT discovery messages, so a device **Zirkulationspumpe** with these entities appears
by itself:

* sensors: line temperature, temperature change, pump (running), the statistics above, WiFi signal, uptime
* button: *Pumpe jetzt einschalten* (same as Enable Pump)
* availability via last will (`circulationpump/status`)

Topics: state `circulationpump/state` (JSON, every 10 s), command `circulationpump/enable/set`
(payload `PRESS`), discovery below `homeassistant/`. MQTT connection attempts back off for 30 s,
the pump control does not depend on the broker.

## Build and flash

PlatformIO, environment `esp32dev`:

```
pio run                 # build
pio run -t upload       # OTA upload to pump.local (espota)
```

Libraries: ArduinoJson 6, OneWire, DallasTemperature, RTClib, PubSubClient.
