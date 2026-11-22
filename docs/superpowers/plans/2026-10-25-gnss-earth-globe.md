# GNSS Earth Globe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add smooth measured-satellite animation and a native Earth globe that animates real GNSS satellite positions from fresh TLE data.

**Architecture:** Keep GSV/GSA parsing and the receiver-relative Sky View as the measured signal path. Add a separate orbit repository that fetches GPS operational TLEs, a small Kotlin orbital propagator, and a Compose globe renderer that projects ECEF positions onto a shaded Earth sphere. The globe displays receiver position from valid GNSS data and labels orbital positions as computed.

**Tech Stack:** Kotlin, Jetpack Compose Canvas, coroutines, Java `HttpURLConnection`, JUnit.

**Spec:** `docs/superpowers/specs/2026-10-25-gnss-earth-globe-design.md`

## Global Constraints

- Existing BLE, GNSS parsing, dashboard, map, debug, history, and Sky View behavior must remain available.
- GSV/GSA measurements must not be presented as orbital positions.
- Computed satellite positions must be labeled `COMPUTED` and must not be treated as GNSS fixes.
- No credentials or private keys may be added to source control.
- Orbital data is optional; the Earth view must remain useful when offline.

## Review Focus

- Invalid or stale TLE data must be rejected without crashing the globe.
- Longitude wrapping at the antimeridian must not create a visual jump in ground tracks.
- Missing GNSS position must remove only the receiver marker, not the Earth globe.
- TLE download failure must preserve cached data and show an offline status.
- The existing Sky View must not begin displaying stale orbital data as measured signal data.

---

### Task 1: Orbital Models and Propagation

**Files:**
- Create: `android/app/src/main/java/com/example/gnssble/data/model/SatelliteOrbit.kt`
- Create: `android/app/src/main/java/com/example/gnssble/data/orbit/SatellitePropagator.kt`
- Create: `android/app/src/test/java/com/example/gnssble/data/orbit/SatellitePropagatorTest.kt`

**Interfaces:**
- `SatelliteOrbit` stores name, NORAD id, epoch, inclination, RAAN, eccentricity, argument of perigee, mean anomaly, and mean motion.
- `SatellitePosition` stores latitude, longitude, altitudeKm, and timestamp.
- `SatellitePropagator.propagate(orbit: SatelliteOrbit, timestampMs: Long): SatellitePosition`.

- [ ] Write tests for deterministic propagation, latitude/longitude bounds, and antimeridian normalization.
- [ ] Run the focused tests and verify they fail because the model/propagator is absent.
- [ ] Implement the model and a bounded two-body TLE-derived propagator suitable for visualization, explicitly documenting that it is approximate and not a navigation fix.
- [ ] Run the focused tests and verify they pass.

### Task 2: Orbit Data Provider

**Files:**
- Create: `android/app/src/main/java/com/example/gnssble/data/orbit/OrbitRepository.kt`
- Create: `android/app/src/test/java/com/example/gnssble/data/orbit/TleParserTest.kt`
- Modify: `android/app/src/main/AndroidManifest.xml`

**Interfaces:**
- `TleParser.parse(text: String): List<SatelliteOrbit>`.
- `OrbitRepository.refresh(): Result<Int>` and `OrbitRepository.current(): StateFlow<OrbitState>`.

- [ ] Write parser tests for valid two-line TLE records, malformed records, and empty input.
- [ ] Run the parser tests and verify the expected failures.
- [ ] Implement parsing, one-hour cache policy, cached fallback, and a CelesTrak GPS operational TLE fetch using `HttpURLConnection`; add only the required Internet permission if absent.
- [ ] Run parser/repository tests and verify they pass.

### Task 3: Smooth Sky View Animation

**Files:**
- Modify: `android/app/src/main/java/com/example/gnssble/ui/screens/SkyViewScreen.kt`
- Create: `android/app/src/test/java/com/example/gnssble/ui/SatelliteInterpolationTest.kt`

- [ ] Write tests for interpolation across azimuth wraparound and missing elevation/azimuth values.
- [ ] Run the focused tests and verify they fail.
- [ ] Add frame-clock animation between the previous and latest measured GSV positions without changing the satellite table semantics.
- [ ] Run the focused tests and verify they pass.

### Task 4: Native Earth Globe

**Files:**
- Create: `android/app/src/main/java/com/example/gnssble/ui/screens/EarthGlobeScreen.kt`
- Create: `android/app/src/main/java/com/example/gnssble/ui/components/EarthGlobeCanvas.kt`
- Modify: `android/app/src/main/java/com/example/gnssble/ui/navigation/GnssNavHost.kt`
- Modify: `android/app/src/main/java/com/example/gnssble/ui/viewmodel/GnssViewModel.kt`

**Interfaces:**
- `EarthGlobeCanvas(orbits: List<SatelliteOrbit>, receiver: GeoPoint?, timestampMs: Long)`.

- [ ] Add a rendering test for receiver-marker visibility with and without valid coordinates.
- [ ] Run the focused test and verify it fails.
- [ ] Implement a shaded orthographic Earth sphere with simplified land silhouettes, grid, receiver marker, computed satellite markers, and short ground-track segments.
- [ ] Add a new `Earth` navigation tab and orbit refresh/status controls.
- [ ] Run the focused test and verify it passes.

### Task 5: Integration and Verification

**Files:**
- Modify: `android/app/src/main/java/com/example/gnssble/ui/screens/EarthGlobeScreen.kt`
- Modify: `android/README.md`

- [ ] Run all Android unit tests.
- [ ] Build the debug APK with `:app:assembleDebug`.
- [ ] Install the APK on the connected device.
- [ ] Verify the app launches, Sky and Earth tabs remain available, and the offline state is readable when TLE download is unavailable.
- [ ] Update README with the distinction between measured Sky View data and computed Earth-orbit data.
