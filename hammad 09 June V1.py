# app.py
import os, csv, queue, threading, datetime
from flask import Flask, request, jsonify, Response
from birdnetlib import Recording
from birdnetlib.analyzer import Analyzer

# ---- your field location (improves accuracy) ----
LAT, LON = 60.17, 24.94     # Helsinki
MIN_CONF = 0.30
CLIP_DIR = "clips"
CSV_FILE = "detections.csv"
os.makedirs(CLIP_DIR, exist_ok=True)

app = Flask(__name__)
print("Loading BirdNET model (takes ~30s the first time)...")
analyzer = Analyzer()
print("Model ready.")

jobs = queue.Queue()
detections = []
lock = threading.Lock()

def worker():
    while True:
        path = jobs.get()
        try:
            rec = Recording(analyzer, path, lat=LAT, lon=LON,
                            date=datetime.datetime.now(), min_conf=MIN_CONF)
            rec.analyze()
            for d in rec.detections:
                row = {
                    "time": datetime.datetime.now().strftime("%H:%M:%S"),
                    "name": d["common_name"],
                    "sci":  d["scientific_name"],
                    "conf": round(d["confidence"], 2),
                }
                with lock:
                    detections.insert(0, row)
                    del detections[200:]
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

@app.route("/upload", methods=["POST"])
def upload():
    data = request.get_data()
    path = os.path.join(CLIP_DIR, f"clip_{datetime.datetime.now():%H%M%S_%f}.wav")
    with open(path, "wb") as f:
        f.write(data)
    jobs.put(path)
    return "ok", 200

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
<body><h1>🐦 Bird Detections <span id=dot style="color:#5fd38a">●</span></h1>
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
    print("Dashboard: http://<this-pi-ip>:5000")
    app.run(host="0.0.0.0", port=5000)
