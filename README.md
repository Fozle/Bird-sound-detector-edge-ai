# 🐦 BirdNET Field Detector (Wired Edition)

Real-time bird sound detection. An **ESP32-S3** with an I²S microphone streams audio over a **USB cable** to a **Raspberry Pi 4**, which runs **BirdNET** to identify species and serves a live web dashboard you can open from your phone or laptop.

The microphone and Pi connect with a single USB cable — no WiFi link between them, no IP addresses to chase. For field use and hand-off to testers, the Pi runs **Comitup**, so anyone can put the device on their own WiFi from their phone — no need to share WiFi passwords with the project owner.

> **Note:** This is the wired successor to an earlier all-WiFi design. See [Why We Switched From WiFi to Wired](#why-we-switched-from-wifi-to-wired).

---

## Table of Contents

- [What It Does](#what-it-does)
- [Architecture](#architecture)
- [Why We Switched From WiFi to Wired](#why-we-switched-from-wifi-to-wired)
- [Hardware](#hardware)
- [Wiring](#wiring)
- [How the USB Audio Link Works](#how-the-usb-audio-link-works)
- [How the Comitup Hotspot Works](#how-the-comitup-hotspot-works)
- [Repository Layout](#repository-layout)
- [Prerequisites](#prerequisites)
- [Setup — Raspberry Pi](#setup--raspberry-pi)
- [Setup — ESP32-S3 Firmware](#setup--esp32-s3-firmware)
- [Running the System](#running-the-system)
- [Testing It (Quick Start)](#testing-it-quick-start)
- [Auto-Start on Boot](#auto-start-on-boot)
- [Detection Accuracy Tuning](#detection-accuracy-tuning)
- [Microphone Sensitivity Tuning](#microphone-sensitivity-tuning)
- [Audio Clip Storage](#audio-clip-storage)
- [The Dashboard](#the-dashboard)
- [Remote Access (Optional)](#remote-access-optional)
- [Command Reference](#command-reference)
- [Troubleshooting](#troubleshooting)
- [Known Constraints](#known-constraints)
- [Roadmap](#roadmap)
- [Acknowledgements](#acknowledgements)

---

## What It Does

1. The microphone continuously listens to the environment.
2. The ESP32 captures the audio in 3-second clips and sends them to the Pi over USB.
3. BirdNET analyzes each clip and identifies any bird species it hears.
4. Detections — species name, scientific name, confidence, timestamp — appear live on a web dashboard and are logged to a CSV file.

Put the device outdoors, sit inside, and watch detected birds appear in your browser.

---

## Architecture

```
┌──────────────┐   I²S    ┌──────────────┐   USB cable     ┌──────────────────┐
│  ICS-43434   │ ───────► │  ESP32-S3    │ ──────────────► │  Raspberry Pi 4  │
│  MEMS mic    │  audio   │  (firmware)  │  serial (PCM)   │  pyserial reader │
└──────────────┘          └──────────────┘                 │  + BirdNET       │
                          powered via USB                   └────────┬─────────┘
                                                                     │ HTTP (LAN)
                                                                     ▼
                                                            ┌──────────────────┐
                                                            │  Web Dashboard   │
                                                            │  (phone/laptop)  │
                                                            └──────────────────┘
```

**Pipeline:**

1. The ICS-43434 produces 24-bit audio on the I²S bus.
2. The ESP32-S3 captures continuous **3-second clips** at 48 kHz, converts them to 16-bit mono PCM, and streams each clip over **USB serial**. The same USB cable also powers the ESP32.
3. A `pyserial` reader thread on the Pi locks onto each clip using an 8-byte sync marker, reassembles the audio, and writes a WAV.
4. A worker thread runs **BirdNET** (via `birdnetlib`) on each clip.
5. Detections are stored in memory and appended to `detections.csv`, and exposed through a JSON API.
6. A self-refreshing web page displays detections live.

> BirdNET runs on the **Raspberry Pi**, not the ESP32-S3. The ESP32 only captures and streams audio; the Pi does all inference.

---

## Why We Switched From WiFi to Wired

### Original design (all WiFi)

Both devices were on WiFi: the **ESP32** `POST`ed clips to the Pi over HTTP, and the **Pi** ran a Flask server receiving them. This worked but was painful:

- **IP addresses kept changing.** The ESP needed the Pi's IP; whenever the router reassigned it (reboot, new network), the ESP couldn't reach the Pi and the firmware had to be **re-flashed**.
- **WiFi credentials were brittle.** New networks meant editing SSID/password on the ESP, and some networks (notably iPhone hotspots) **block mDNS**, so name resolution failed entirely.
- **Two devices, two failure points.**

### Current design (wired)

We replaced the **ESP → Pi** WiFi hop with a **direct USB cable**. The ESP streams audio as serial bytes; the Pi reads them with `pyserial`; the cable also powers the ESP.

| Problem (WiFi) | Result (Wired) |
| --- | --- |
| Pi IP changes → re-flash ESP | ESP has **no IP at all** — nothing to reconfigure |
| WiFi credentials on the ESP | ESP needs **no WiFi** — works anywhere |
| mDNS blocked on some hotspots | Irrelevant — no name resolution involved |
| Two WiFi failure points | One physical cable; deterministic |

**Trade-off:** a USB cable tethers the mic to the Pi (~5 m max), so the two sit close together. Audio quality is unchanged (48 kHz) because the ESP32-S3's native USB has ample bandwidth.

**What still uses WiFi:** only the Pi, and only so you can *view* the dashboard. The audio path is fully wired.

---

## Hardware

| Component       | Model / Spec                  | Notes                                          |
| --------------- | ----------------------------- | ---------------------------------------------- |
| Microcontroller | ESP32-S3 (dev board)          | Has **two** ports: `USB` (native) and `UART`   |
| Microphone      | ICS-43434 (I²S MEMS)          | 24-bit; wiring identical to INMP441            |
| Server          | Raspberry Pi 4                | Running **64-bit** Raspberry Pi OS             |
| Link            | 1× USB data cable             | ESP `USB` port → any Pi USB port               |

---

## Wiring

ICS-43434 → ESP32-S3. The `SEL` pin is tied to **GND** to select the **left** I²S channel.

| Mic Pin | ESP32-S3 GPIO | Purpose                  |
| ------- | ------------- | ------------------------ |
| SEL     | GND           | Channel select (LEFT)    |
| LRCL    | GPIO 17       | Word select / L-R clock  |
| DOUT    | GPIO 18       | Serial data out          |
| BCLK    | GPIO 8        | Bit clock                |
| GND     | GND           | Ground                   |
| 3V      | 3V3           | Power                    |

**ESP32-S3 → Raspberry Pi:** a single USB cable from the ESP32's **`USB`** connector (native USB) to **any** USB port on the Pi.

> ⚠️ **Critical:** the ESP32-S3 has two USB connectors. Use the **`USB`** port to connect to the Pi — **not** the `UART` port. The `UART` port is for flashing only and carries no application data; using it makes the Pi see the device but receive **zero bytes**.

---

## How the USB Audio Link Works

Audio is sent over USB serial using a simple, robust framing protocol. Each clip is one frame:

```
┌─────────────────┬──────────────┬───────────────┬─────────────────────┐
│  8-byte marker  │  sample rate │  sample count │   PCM audio data    │
│ AB CD EF 12     │  (uint32 LE) │  (uint32 LE)  │  (16-bit mono LE)   │
│ 34 56 78 9A     │              │               │  count × 2 bytes    │
└─────────────────┴──────────────┴───────────────┴─────────────────────┘
```

- The **8-byte sync marker** lets the receiver find the start of each clip, even if it connects mid-stream.
- **Header bytes are written one at a time** on the ESP32. This is essential: the ESP32-S3's native USB (TinyUSB CDC) can silently drop *small* writes that follow a large bulk transfer. Sending the 16-byte header byte-by-byte (with a flush) guarantees the marker survives, while the bulk audio is written in blocks.
- The Pi scans for the marker, reads the header, then reads exactly `count × 2` bytes of PCM and writes a WAV.

This is what makes a continuous, self-recovering stream possible over a link that re-enumerates on every ESP reset.

---

## How the Comitup Hotspot Works

For field use and hand-off to testers, the Pi runs **[Comitup](https://davesteele.github.io/comitup/)**. This solves a hard problem: *how does someone put the headless Pi on their own WiFi when you don't know their network and can't ask everyone for their password?*

**The behavior (just like a smart-home device's first-time setup):**

1. On power-up, the Pi tries to join a **known** WiFi network.
2. If it finds one → it connects normally (e.g. your home network). Nothing else happens.
3. If it **can't** find any known network → it automatically starts its **own WiFi hotspot** named **`BirdNET-<nnn>`** (e.g. `BirdNET-553`).

**How a tester connects it to their WiFi:**

1. Power on the Pi (plug in the ESP via the USB port too). Wait ~1–2 minutes.
2. On their phone, open WiFi settings and connect to **`BirdNET-553`**.
3. A configuration page opens automatically (a captive portal). If it doesn't, browse to **`http://10.41.0.1`**.
4. The page lists nearby WiFi networks — they pick their own and enter its password.
5. The Pi joins that network, remembers it, and the hotspot disappears.
6. From then on, the Pi auto-connects to that network on every boot.

**Why this matters for hand-off:** each tester configures the Pi with *their own* WiFi, from their phone, in under a minute. The project owner never needs anyone's WiFi credentials. This mirrors the original ESP's WiFiManager portal behavior, now applied to the Pi.

> Comitup co-exists with NetworkManager (the default on Raspberry Pi OS Bookworm), so your normal home WiFi keeps working — the hotspot only appears when no known network is in range.

---

## Repository Layout

> Adjust to match your actual file names.

```
.
├── firmware/
│   └── usb_streamer/          # ESP32-S3 sketch: streams audio over USB serial
├── server/
│   └── app.py                 # Pi: serial reader + BirdNET + dashboard + clip cleanup
├── tools/
│   └── serial_test.py         # Standalone receiver to verify the wired link
├── docs/
│   ├── screenshots/           # Dashboard / serial / server reference images
│   └── BirdNET_Commands.docx  # Full command reference
└── README.md
```

---

## Prerequisites

**Development machine (PC):**

- [Arduino IDE 2.x](https://www.arduino.cc/en/software) with the **ESP32 board package** (Arduino-ESP32 core **3.x** — required for the `i2s_std` driver)

**Raspberry Pi:**

- **Raspberry Pi OS (64-bit)** — ⚠️ 32-bit will **not** work (TensorFlow Lite has no 32-bit wheels)

---

## Setup — Raspberry Pi

SSH into the Pi from your PC:

```bash
ssh <user>@<pi-ip>
```

### 1. System dependencies

```bash
sudo apt update
sudo apt install -y python3-venv python3-dev ffmpeg libsndfile1
```

> `libatlas-base-dev` is **not** required on recent Pi OS (dropped from the repos; TensorFlow bundles its own math routines).

### 2. Python environment

Recent Pi OS enforces [PEP 668](https://peps.python.org/pep-0668/), so a virtual environment is mandatory:

```bash
python3 -m venv ~/birdnet-env
source ~/birdnet-env/bin/activate
pip install --upgrade pip
pip install flask birdnetlib librosa pyserial
pip install tensorflow
```

The `tensorflow` install is large and may take **5–15 minutes** on a Pi 4.

### 3. Python 3.13 compatibility fix (important)

If your Pi OS ships **Python 3.13**, TensorFlow fails to import with `ModuleNotFoundError: No module named 'imp'` — the bundled `flatbuffers` relies on `imp`, removed in Python 3.12+. Force a newer version:

```bash
python -m pip install --force-reinstall "flatbuffers==25.2.10"
python -c "import flatbuffers, flatbuffers.compat; print('flatbuffers OK')"
```

### 4. Serial port access

```bash
sudo usermod -a -G dialout <user>
sudo reboot
```

### 5. Comitup (field hotspot — optional but recommended for hand-off)

```bash
sudo apt update
sudo apt install -y comitup
sudo nano /etc/comitup.conf      # set:  ap_name: BirdNET-<nnn>
sudo systemctl enable comitup
sudo reboot
```

Test at home first to confirm your normal WiFi still connects after install.

### 6. Deploy the server

Place `app.py` in the home directory (`~/app.py`). Confirm the settings near the top match your setup (serial port, tuning) — see below.

---

## Setup — ESP32-S3 Firmware

> **Golden rule:** **flash via the `UART` port; connect to the Pi via the `USB` port.**

1. Connect the ESP32 to your **PC using the `UART` port**.
2. In Arduino IDE set:
   - **Board:** ESP32S3 Dev Module
   - **Port:** the ESP's COM port
   - **USB CDC On Boot: → Enabled** ← required; without it, `Serial` goes to the UART pins and the Pi receives nothing
3. Open the streamer sketch and **Upload**. If it hangs on *"Connecting…"*, hold **BOOT**, tap **RESET**, release **BOOT**, upload again.
4. Move the cable to the ESP's **`USB`** port and plug into the Pi.

---

## Running the System

**1. Stop the auto-start service if it's holding the port:**

```bash
sudo systemctl stop birdnet.service
```

**2. Start the server:**

```bash
source ~/birdnet-env/bin/activate
python ~/app.py
```

Wait for:

```
Model ready.
Listening on /dev/ttyACM0
 * Running on http://0.0.0.0:5000
  (no bird this clip)
```

`(no bird this clip)` every few seconds is normal — clips are arriving and being analyzed; the area is just quiet.

**3. Open the dashboard** from any device on the same network:

```
http://<pi-ip>:5000
```

> **Port name note:** the serial device can appear as `/dev/ttyACM0` or `/dev/ttyACM1` and may change on reconnect. If the server can't find it, run `ls /dev/ttyACM*`, update the `PORT` value at the top of `app.py`, and restart.

---

## Testing It (Quick Start)

You don't need real birds to test — just play a bird call near the microphone.

**▶️ Test audio (bird sounds):** https://www.youtube.com/watch?v=HCp9oSz0sVc

**Steps:**

1. Make sure the system is running (`app.py` active, dashboard open at `http://<pi-ip>:5000`).
2. Play the video above on a phone or speaker, **held close to the microphone**, with the volume up.
3. Within a few seconds, the Pi log prints a detection (`BIRD: <name> (0.xx)`) and a new row appears on the dashboard with the species name, scientific name, confidence, and time.

**What you should see:**

- **A row appears** → the full pipeline works: mic → USB → Pi → BirdNET → dashboard. ✅
- **Only `(no bird this clip)`** → the audio is too quiet or unclear. Move the speaker closer/louder, or lower `MIN_CONF` (see tuning). Phone speakers are imperfect, so don't worry if the exact species isn't right — a row landing is the win.

> Tip: detection quality depends on audio quality. A clear, loud call close to the mic detects reliably; faint or distant audio may not. This is normal for any acoustic detection system.

---

## Auto-Start on Boot

Run the server automatically on every boot — no SSH session needed.

```bash
sudo nano /etc/systemd/system/birdnet.service
```

```ini
[Unit]
Description=BirdNET bird detection server (wired)
After=multi-user.target

[Service]
User=<user>
WorkingDirectory=/home/<user>
ExecStart=/home/<user>/birdnet-env/bin/python /home/<user>/app.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable birdnet.service
sudo systemctl start birdnet.service
sudo systemctl status birdnet.service   # expect: active (running)
```

The reader thread retries until the ESP's serial port appears, so it tolerates the ESP being plugged in after boot.

---

## Detection Accuracy Tuning

Edit these lines near the top of `app.py`, then restart the service. **`MIN_CONF` is the main control.**

```python
MIN_CONF = 0.25            # threshold: HIGHER = fewer false detections (try 0.3–0.5)
USE_LOCATION = False       # True = restrict to locally-plausible species; False = all species
LAT, LON = 60.17, 24.94    # your location (used only when USE_LOCATION = True)
```

- **`MIN_CONF`** — the minimum confidence (0.0–1.0) before a detection is reported. It filters *all* false positives. Raise it (0.3–0.5) to cut noise; lower it (0.15) to catch fainter birds at the cost of more errors. The acoustic analysis is identical at any threshold — this only changes what gets reported.
- **`USE_LOCATION`** — an *optional* filter that suppresses species implausible for your location/season (e.g. a North American bird flagged in Finland). It does **not** improve analysis; it only removes "impossible" results. Turn it on if you see geographically wrong species.

**To stop false detections:** set `MIN_CONF = 0.5` and `USE_LOCATION = True`, then restart.

```bash
sudo systemctl restart birdnet.service
```

---

## Microphone Sensitivity Tuning

Edit this one line in the ESP sketch, then re-flash (via the `UART` port):

```cpp
#define GAIN 4       // higher = more sensitive (more faint birds, but more noise/false hits)
```

Guide: `4` = balanced, `8` = sensitive, `16` = very sensitive (clipping risk). Raising sensitivity also amplifies background noise, so if you have *false* detections, lower it rather than raise it. Apply with: edit → flash via UART → move cable to USB → restart the service.

---

## Audio Clip Storage

Each received clip is saved as a WAV in `~/clips/`. To prevent the SD card filling up, `app.py` keeps only the newest clips:

```python
KEEP_CLIPS = 20            # how many recent clips to keep on disk
```

Older clips are deleted automatically as new ones arrive. The detection log (`detections.csv`) is plain text and is never trimmed. To copy clips to your PC for listening, use `scp` (see Command Reference).

---

## The Dashboard

A single self-contained page served at `http://<pi-ip>:5000`, polling `/api/detections` every 3 seconds.

| Column     | Description                      |
| ---------- | -------------------------------- |
| Time       | Detection timestamp (`HH:MM:SS`) |
| Bird       | Common name                      |
| Scientific | Scientific name                  |
| Conf       | Model confidence (0.00–1.00)     |

### API

| Endpoint            | Method | Description                                  |
| ------------------- | ------ | -------------------------------------------- |
| `/`                 | GET    | The dashboard page                           |
| `/api/detections`   | GET    | JSON array of recent detections (max 200)    |

Detections also persist to `detections.csv`; raw clips are saved under `clips/` (capped at `KEEP_CLIPS`).

---

## Remote Access (Optional)

By default the dashboard is reachable only on the local network. To view it from **anywhere** (mobile data, another city), install [**Tailscale**](https://tailscale.com) — a private encrypted mesh network. **Do not use router port forwarding**, which exposes an unauthenticated dev server to the public internet.

```bash
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up          # opens a login URL; sign in to join your network
tailscale ip -4            # shows a stable 100.x.x.x address
```

Install the Tailscale app on your phone/laptop, sign in with the **same account**, and browse to `http://100.x.x.x:5000` from anywhere. This is especially useful for hand-off: combined with Comitup, a tester gets the Pi online via the hotspot, and you can view results remotely regardless of their network.

---

## Command Reference

**Connect & run**
```bash
ssh <user>@<pi-ip>                       # connect from PC (PowerShell)
hostname -I                              # (on Pi) show current IP
source ~/birdnet-env/bin/activate        # activate environment (ALWAYS first)
python ~/app.py                          # run server manually
```

**Service control**
```bash
sudo systemctl start|stop|restart birdnet.service
sudo systemctl status birdnet.service    # q to exit
sudo systemctl enable|disable birdnet.service
journalctl -u birdnet.service -f         # live log (Ctrl+C to stop)
```

**Detections, logs, clips**
```bash
cat ~/detections.csv                     # all detections
tail -20 ~/detections.csv                # recent
ls ~/clips/ | wc -l                      # clip count (stays ~20)
rm ~/clips/*.wav                         # clear clips
```

**Copy files to PC** (run in PowerShell, not SSH)
```powershell
scp <user>@<pi-ip>:~/detections.csv C:\path\
scp -r <user>@<pi-ip>:~/clips C:\path\clips
```

**Serial port**
```bash
ls /dev/ttyACM*                          # find ESP port; update PORT in app.py if needed
```

**WiFi**
```bash
sudo nmtui                                                  # menu WiFi manager
sudo nmcli device wifi connect "SSID" password "PASSWORD"   # join + save
```

**Comitup**
```bash
sudo systemctl status comitup
cat /etc/comitup.conf
comitup-cli                              # current mode / networks (q to exit)
```

**BirdNET package** (environment activated)
```bash
pip show birdnetlib                                   # version + location
pip install birdnetlib==0.18.1                        # roll back to known-good
pip install --force-reinstall --no-deps birdnetlib    # safe repair (keeps tensorflow)
```

**Full rebuild from scratch**
```bash
sudo apt update
sudo apt install -y python3-venv python3-dev ffmpeg libsndfile1
python3 -m venv ~/birdnet-env
source ~/birdnet-env/bin/activate
pip install --upgrade pip
pip install flask birdnetlib librosa pyserial
pip install tensorflow
python -m pip install --force-reinstall "flatbuffers==25.2.10"
```

---

## Troubleshooting

| Symptom                                              | Cause / fix                                                                                                   |
| ---------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| Pi sees `/dev/ttyACM*` but receives **0 bytes**      | Cable is in the ESP's **UART** port — move it to the **USB** port. Also confirm **USB CDC On Boot = Enabled**. |
| Marker never found / no clips decode                 | Header was dropped by USB-CDC — ensure firmware sends header bytes **one at a time** (current sketch does).    |
| `app.py`: serial error / port not found              | Port name changed — `ls /dev/ttyACM*`, update `PORT` in `app.py`, restart.                                    |
| `ModuleNotFoundError: No module named 'imp'`         | Python 3.13 issue — `pip install --force-reinstall "flatbuffers==25.2.10"`.                                   |
| `Address already in use` on port 5000                | The service is already running — `sudo systemctl stop birdnet.service` before running manually.               |
| `error: externally-managed-environment` from pip     | Activate the venv first: `source ~/birdnet-env/bin/activate`.                                                 |
| Too many false detections                            | Raise `MIN_CONF` (0.3–0.5); set `GAIN = 4`; enable `USE_LOCATION` if species are impossible.                  |
| Faint/clear birds missed                             | Lower `MIN_CONF` (0.15–0.2); raise `GAIN` moderately; shield the mic from wind.                               |
| Can't SSH (timed out)                                | Pi's IP changed — check the router's device list for its new IP.                                              |
| Pi not on network after reboot                       | Power-cycle and wait ~3 min. If field-deployed, use the Comitup hotspot to reconnect it.                      |
| Tester can't get Pi online                           | Connect phone to `BirdNET-<nnn>` hotspot → portal (or `http://10.41.0.1`) → pick WiFi + password.             |

---

## Known Constraints

- **USB tether (~5 m).** A wired link keeps the microphone close to the Pi.
- **Raspberry Pi OS must be 64-bit** for TensorFlow Lite / BirdNET.
- **Serial port name is not fixed** (`ttyACM0`/`ttyACM1`) and can change on reconnect; update `PORT` in `app.py` if needed (a udev rule can pin it permanently).
- **Dashboard is unauthenticated** and uses the Flask development server — keep it on a trusted network or front it with Tailscale, not public port forwarding.
- **Detection quality is capped by audio quality.** Distance, wind, and gain all matter; no acoustic system catches every bird.

---

## Roadmap

- [ ] Pin the serial device name with a udev rule (no more `ACM0`/`ACM1` surprises)
- [ ] Built-in remote dashboard access via Tailscale
- [ ] Play back detection audio clips from the dashboard
- [ ] Charts and historical analytics (detections per species / per hour)
- [ ] Production WSGI server in place of the Flask dev server
- [ ] Optional WiFi transport fallback for untethered deployments

---

## Acknowledgements

- [BirdNET](https://birdnet.cornell.edu/) — Cornell Lab of Ornithology & Chemnitz University of Technology
- [`birdnetlib`](https://pypi.org/project/birdnetlib/) — Python interface to BirdNET
- [Comitup](https://davesteele.github.io/comitup/) — headless WiFi hotspot configuration
