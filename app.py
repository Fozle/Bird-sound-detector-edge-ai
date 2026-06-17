import os, csv, struct, wave, queue, threading, datetime, time, serial
from flask import Flask, jsonify, Response
from birdnetlib import Recording
from birdnetlib.analyzer import Analyzer

PORT = "/dev/ttyACM0"      # match `ls /dev/ttyACM*`
MAGIC = bytes([0xAB,0xCD,0xEF,0x12,0x34,0x56,0x78,0x9A])
MIN_CONF = 0.25            # lower = more detections (and more false positives)
USE_LOCATION = False       # False = consider all species; True = filter by LAT/LON
LAT, LON = 60.17, 24.94
CLIP_DIR = "clips"
CSV_FILE = "detections.csv"
KEEP_CLIPS = 20            # how many recent clips to keep on disk
os.makedirs(CLIP_DIR, exist_ok=True)

print("Loading BirdNET model...")
analyzer = Analyzer()
print("Model ready.")

app = Flask(__name__)
jobs = queue.Queue()
detections = []
lock = threading.Lock()

def read_exact(ser, n):
    b = bytearray()
    while len(b) < n:
        c = ser.read(n - len(b))
        if c: b.extend(c)
    return bytes(b)

def sync(ser):
    w = bytearray()
    while True:
        x = ser.read(1)
        if not x: continue
        w.extend(x)
        if len(w) > 8: del w[0]
        if bytes(w) == MAGIC: return

def trim_clips():
    # keep only the KEEP_CLIPS newest .wav files in CLIP_DIR
    clips = sorted(
        [os.path.join(CLIP_DIR, f) for f in os.listdir(CLIP_DIR) if f.endswith(".wav")],
        key=os.path.getmtime
    )
    for old in clips[:-KEEP_CLIPS]:
        try:
            os.remove(old)
        except OSError:
            pass

def reader():
    while True:
        try:
            ser = serial.Serial(PORT, 115200, timeout=1)
            print("Listening on", PORT)
            while True:
                sync(ser)
                sr, n = struct.unpack("<II", read_exact(ser, 8))
                if not (8000 <= sr <= 48000 and 1 <= n <= sr*10):
                    continue
                pcm = read_exact(ser, n * 2)
                path = os.path.join(CLIP_DIR, f"clip_{datetime.datetime.now():%H%M%S_%f}.wav")
                w = wave.open(path, "wb"); w.setnchannels(1); w.setsampwidth(2)
                w.setframerate(sr); w.writeframes(pcm); w.close()
                trim_clips()                       # keep only the newest KEEP_CLIPS clips
                if jobs.qsize() < 5:
                    jobs.put(path)
        except serial.SerialException as e:
            print("Serial error:", e, "- retrying")
            time.sleep(2)

def worker():
    while True:
        path = jobs.get()
        try:
            if USE_LOCATION:
                rec = Recording(analyzer, path, lat=LAT, lon=LON,
                                date=datetime.datetime.now(), min_conf=MIN_CONF)
            else:
                rec = Recording(analyzer, path, min_conf=MIN_CONF)
            rec.analyze()
            for d in rec.detections:
                row = {"time": datetime.datetime.now().strftime("%H:%M:%S"),
                       "name": d["common_name"], "sci": d["scientific_name"],
                       "conf": round(d["confidence"], 2)}
                with lock:
                    detections.insert(0, row); del detections[200:]
                with open(CSV_FILE, "a", newline="") as f:
                    csv.writer(f).writerow([row["time"], row["name"], row["sci"], row["conf"]])
                print(f"  BIRD: {row['name']} ({row['conf']})")
            if not rec.detections:
                print("  (no bird this clip)")
        except Exception as e:
            print("  analyze error:", e)
        finally:
            jobs.task_done()

threading.Thread(target=worker, daemon=True).start()
threading.Thread(target=reader, daemon=True).start()

@app.route("/api/detections")
def api():
    with lock:
        return jsonify(detections)

@app.route("/")
def dashboard():
    return Response("""<!doctype html><html><head><meta charset=utf-8>
<title>Bird Detections</title><meta name=viewport content="width=device-width,initial-scale=1">
<style>body{font-family:system-ui;margin:0;background:#0f1419;color:#e6e6e6}
h1{padding:18px;margin:0;background:#1a2530;font-size:20px}
table{width:100%;border-collapse:collapse}td,th{padding:12px 18px;text-align:left;border-bottom:1px solid #243240}
th{color:#6fb3ff;font-size:13px;text-transform:uppercase}
tr:hover{background:#16202b}.conf{color:#5fd38a;font-weight:600}
.empty{padding:40px;text-align:center;color:#5a6b7a}</style></head>
<body><h1>🐦 Bird Detections</h1>
<table><thead><tr><th>Time</th><th>Bird</th><th>Scientific</th><th>Conf</th></tr></thead>
<tbody id=rows><tr><td class=empty colspan=4>Listening…</td></tr></tbody></table>
<script>
async function load(){
  try{
    const r=await fetch('/api/detections'); const d=await r.json();
    const t=document.getElementById('rows');
    if(!d.length){t.innerHTML='<tr><td class=empty colspan=4>Listening…</td></tr>';return;}
    t.innerHTML=d.map(x=>`<tr><td>${x.time}</td><td>${x.name}</td>
      <td><i>${x.sci}</i></td><td class=conf>${x.conf}</td></tr>`).join('');
  }catch(e){}
}
load(); setInterval(load,3000);
</script></body></html>""", mimetype="text/html")

if __name__ == "__main__":
    print("Dashboard: http://<pi-ip>:5000")
    app.run(host="0.0.0.0", port=5000)
