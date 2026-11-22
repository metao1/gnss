# GNSS Serial Android App

Professional Android client for the nRF9151 GNSS stream routed through the Yahboom HC-06 Bluetooth 2.0 serial module.

## Hardware Path

```text
nRF9151 P0.29 TX -> 4WD PA2 / Bluetooth RX
nRF9151 P0.28 RX <- 4WD PA3 / Bluetooth TX
nRF9151 GND      -> 4WD GND
UART baud        -> 9600
Bluetooth module -> HC-06, PIN 1234
```

## App Behavior

- Lists already paired Bluetooth Classic devices.
- Connects to HC-06 using SPP UUID `00001101-0000-1000-8000-00805F9B34FB`.
- Reads newline-delimited JSON from the nRF9151 firmware.
- Sends commands with newline terminators: `PING`, `REQUEST_STATUS`, `START_STREAM`, `STOP_STREAM`, `SET_RATE:<1-5>`.
- Stores sessions locally with Room.
- Saves only valid GNSS fixes as track points.
- Keeps raw serial/status/error lines in the debug log.

## First Run

1. Pair Android with `HC-06` in system Bluetooth settings.
2. Use PIN `1234`.
3. Open this app and grant Bluetooth connect permission if Android asks.
4. Tap `Refresh Paired`.
5. Select `HC-06`, then tap `Connect Serial`.
6. Tap `Ping`; expect a `PONG` status JSON in Debug.

## Build

Open `android/` in Android Studio and run the `app` configuration. The map uses OpenStreetMap tiles through osmdroid and does not require a Google Maps API key. The map displays required OpenStreetMap attribution and needs internet access for live tiles.

## Gradle Version Note

Build this project with the checked-in Gradle wrapper from the `android/` directory:

```bash
./gradlew :app:assembleDebug
```

Do not build it with a global Gradle 9.x installation. AGP `8.5.2` and the current Kotlin/Hilt stack are pinned to Gradle `8.7`; using Gradle 9.x can fail during project configuration with:

```text
Configuration.fileCollection(org.gradle.api.specs.Spec)
```

In Android Studio, open the `android/` folder and set Gradle JDK/project settings to use the Gradle wrapper.

## Runtime Permissions

The app requests Bluetooth and Location permissions dynamically before refresh or connection:

- Android 12+: `BLUETOOTH_CONNECT` and `BLUETOOTH_SCAN` appear as Nearby Devices permissions.
- Android 13+: `POST_NOTIFICATIONS` is requested so the active serial-session notification can be shown.
- All Android versions: `ACCESS_FINE_LOCATION` is requested explicitly so Location appears in the app permission flow. On Android 12+, Bluetooth itself appears as Nearby Devices, but this app still asks for Location for compatibility and map/session features.

The app still lists paired HC-06 devices only; pairing itself happens in Android Bluetooth settings.
# Satellite Views

The **Sky** tab shows measured receiver data from the BLE GNSS stream. It uses
GSV/GSA azimuth, elevation, C/N0, and used-state values and smoothly animates
markers between received updates.

The **Earth** tab is a separate orbital visualization. It downloads GPS
operational TLE data when online, caches it locally for one hour, and uses an
approximate two-body propagator to animate computed satellite positions and
short ground tracks around a shaded Earth globe. Computed markers are not
GNSS fixes and must not be used for navigation. The receiver marker appears
only when the device has a valid GNSS position.

If TLE download fails, cached orbital data remains available. With no cache,
the Earth view remains visible and asks you to press **Refresh** after network
connectivity is restored.
