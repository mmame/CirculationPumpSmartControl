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
* The pump runs in every slot whose counter is above 0, **shifted earlier by the pump lead time**
  (`/config`, default 5 min, 0 = off): it switches on `lead` minutes before an active slot begins
  and off `lead` minutes before it ends, so the water is already circulated when the slot starts and
  is still warm at its end. The total run time does not change. The shift is computed from the
  local time of *now + lead*, so it also works across midnight, across the week change
  (Saturday 23:55 -> Sunday slot 0) and at daylight saving time changes.
* The **Enable Pump** button (web UI, Home Assistant) raises the counter of the slot that is
  currently controlling the pump by one and so starts the pump right away.
* The usage grid can be viewed, edited and reset in the web UI.

## Web UI

Open `http://pump.local/` (or the IP address).

| Page | Purpose |
|---|---|
| `/` | status, running share, Enable Pump, restart |
| `/grid` | edit the learned usage pattern |
| `/config` | thresholds and timing |
| `/wificonfig` | WiFi credentials (without credentials the device opens the access point `CirculationPumpSmartControl`) |
| `/timeconfig` | set the RTC |
| `/mqttconfig` | MQTT / Home Assistant |
| `/update` | firmware upload |

JSON endpoints: `GET /getstatus`, `GET /getstats`, `POST /enablepump`.

## Running share

The only statistic is the **weekly running share**: the percentage of the week (7 x 96 slots of
15 minutes) in which the learned pattern runs the pump. It is shown on the start page, in
`GET /getstats` and in Home Assistant, and is deterministic - it follows directly from the usage grid.

Measured run times, a hit/miss rate or a "wasted" run time are deliberately **not** reported.
Everything derived from the hot water usage detection would be misleading: the detection misses a
usage that follows shortly after another one, because the inlet temperature has already dropped. (An
earlier "wasted" value was dominated by the `increaseOnUsage` setting: with 7 it tends towards
7/(7+1) = 87.5 % regardless of the real behaviour.)

## Home Assistant (MQTT)

Configure the broker under `/mqttconfig` (host, port, user, password). The device then publishes
Home Assistant MQTT discovery messages, so a device **Zirkulationspumpe** with these entities appears
by itself:

* sensors: line temperature, temperature change, pump (running), running share (week), WiFi signal, uptime
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
