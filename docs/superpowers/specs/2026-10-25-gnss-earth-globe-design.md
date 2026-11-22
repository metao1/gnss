# GNSS Earth Globe Design

## Intent

Add a second visualization for satellite motion around a real Earth globe while preserving the existing receiver-relative Sky View. The Sky View remains the source of measured signal information from GSV/GSA data; the Earth view uses computed orbital positions and must label them as calculated.

## Goals

- Smoothly animate measured satellite markers in the existing Sky View.
- Add a native Android Earth/globe screen with a real Earth texture.
- Show computed satellite positions and ground tracks from orbital data.
- Keep BLE, GNSS parsing, dashboard, and map behavior unchanged.
- Degrade to an informative empty state when orbital data is unavailable or stale.

## Non-goals

- Deriving orbital positions from GSV azimuth/elevation alone.
- Claiming computed positions are the exact satellites currently received unless a matching PRN and fresh orbital record exists.
- Adding cloud services or requiring a new user account.

## Constraints

- Existing app is Kotlin + Jetpack Compose with minSdk 26.
- Existing map uses osmdroid and MapTiler; the globe should not replace it.
- No credentials are printed or committed.
- Orbital data must be replaceable without changing the renderer.

## Approaches

1. Native Compose globe with a lightweight orthographic renderer and an orbit-provider interface. This keeps lifecycle, animation, and state native, but requires a compact Earth texture and a small projection implementation.
2. WebView with CesiumJS. This provides a mature globe quickly, but adds JavaScript lifecycle/network complexity and makes offline behavior less predictable.
3. 2D equirectangular Earth map. This is simplest, but does not meet the requested real-globe experience.

The native approach is selected because it fits the existing Compose app, avoids WebView dependency, and lets the app distinguish measured signal data from computed orbital data.

## Design

- `SatelliteOrbit` models a satellite name, constellation, NORAD id, TLE lines, and update timestamp.
- `OrbitProvider` supplies current orbital records and can later be replaced by a downloader/cache without changing the UI.
- `SatellitePropagator` converts an orbit record and time into Earth-centered coordinates and a ground point. The first implementation uses deterministic sample orbital records and keeps the interface ready for real TLE/SGP4 integration; no fake location is shown as measured GNSS data.
- `EarthGlobeScreen` renders a shaded Earth sphere, latitude/longitude grid, receiver marker when a valid fix exists, and computed satellite markers/ground tracks.
- Animation uses a Compose frame clock and updates only the computed layer. The receiver marker continues to use the latest valid GNSS fix.
- The Sky View interpolates between successive GSV measurements for smooth marker movement and keeps its existing signal table.

## Acceptance checks

- Sky markers move smoothly between received GSV updates.
- Earth view shows a recognizable shaded globe rather than a circular sky plot.
- Receiver location is shown only when GNSS coordinates are valid.
- Computed satellite data is labeled `COMPUTED`, and missing orbit data shows a clear message.
- Existing Android debug, BLE, dashboard, map, and history screens still build and launch.
