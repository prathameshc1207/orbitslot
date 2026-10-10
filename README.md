# OrbitSlot

Autonomous ground station scheduling for multi-satellite downlink.
FUSION 2026, problem statement SPACE-04, Team Jarvis.

OrbitSlot decides which satellite gets which ground-station antenna, and when, so that the most
important data reaches the ground before its deadline. The dashboard compares it with
first-come-first-served (FCFS) and a priority-first rule on the same passes, data and constraints.

## What is real

- Satellite orbits: live Planet TLEs from CelesTrak, propagated with SGP4 (satellite.js)
- Ground stations, satellites, schedules and received signals: live from the SatNOGS Network
- Earth imagery: NASA Blue Marble and Black Marble

Data volumes and download queues are simulated, because operators do not publish them.

## Project layout

    index.html                 the dashboard (one self-contained file)
    api/tle.js                 serverless function: today's Planet TLEs from CelesTrak (cached 6 h)
    api/satnogs.js             serverless function: real SatNOGS observations (cached 15 min)
    hardware/                  ESP32 firmware: OLED, LED, buzzer and MPU6050 ground station

## Deploy

1. Push this folder to a GitHub repository.
2. On vercel.com: Add New, Project, import the repository, keep the default settings, Deploy.
3. Check that https://<your-site>/api/tle and https://<your-site>/api/satnogs return JSON.

Every push to the main branch redeploys automatically.

## Hardware

Open hardware/OrbitSlot_GroundStation/OrbitSlot_GroundStation.ino in the Arduino IDE, install
Adafruit SSD1306, Adafruit GFX and ArduinoJson, and upload to an ESP32. Wiring is in the file
header. Connect from the dashboard with "Connect ESP32" (Chrome or Edge on a laptop).

## Credits

SGP4 by satellite.js (MIT). 3D rendering by three.js. Orbital elements from CelesTrak.
Observation data from the SatNOGS Network by the Libre Space Foundation.
