#ifndef WEBPAGE_H
#define WEBPAGE_H

#include <pgmspace.h>

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pl">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>GPS Tracker V3</title>
    <!-- Map & Charts -->
    <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" />
    <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    
    <style>
        :root { --bg: #121212; --surf: #1e1e1e; --primary: #bb86fc; --sec: #03dac6; --warn: #cf6679; --text: #eee; }
        * { box-sizing: border-box; }
        body { margin:0; font-family:sans-serif; background:var(--bg); color:var(--text); display:flex; flex-direction:column; height:100vh; overflow:hidden; }
        
        /* HEADER status bar */
        header { background:var(--surf); padding:10px; display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid #333; z-index:10; }
        .mode-ind { font-weight:bold; font-size:0.8rem; padding:4px 8px; border-radius:4px; }
        .mode-live { background:#2ecc71; color:#000; }
        .mode-view { background:#3498db; color:#fff; }
        .conn-stat { font-size:0.7rem; color:#888; }

        /* TABS */
        .tabs { display:flex; background:#000; }
        .tab { flex:1; padding:12px; background:none; border:none; color:#777; font-weight:bold; border-bottom:3px solid transparent; }
        .tab.active { color:var(--sec); border-bottom-color:var(--sec); }

        /* CONTENT */
        .page { flex:1; overflow-y:auto; padding:10px; display:none; }
        .page.active { display:block; }

        /* DASHBOARD */
        .grid { display:grid; grid-template-columns:1fr 1fr; gap:8px; margin-bottom:10px; }
        .card { background:var(--surf); padding:10px; border-radius:8px; text-align:center; position:relative; }
        .val { font-size:1.4rem; font-weight:bold; }
        .lbl { font-size:0.7rem; color:#aaa; text-transform:uppercase; }
        .full { grid-column:span 2; }
        
        /* MAP */
        #map-box { height:300px; background:#222; border-radius:8px; margin-bottom:10px; position:relative; overflow:hidden; }
        #map { height:100%; width:100%; }
        .pause-overlay { position:absolute; top:50%; left:50%; transform:translate(-50%,-50%); background:rgba(255, 165, 0, 0.9); color:#000; padding:10px 20px; font-weight:bold; border-radius:8px; z-index:1000; display:none; animation:blink 1.5s infinite; }
        @keyframes blink { 50% { opacity:0.7; } }

        /* CHARTS */
        .charts-row { display:grid; grid-template-columns:1fr 1fr; gap:5px; height:120px; margin-bottom:10px; }
        .chart-wrap { background:var(--surf); border-radius:8px; padding:2px; position:relative; }
        
        /* FILES */
        .file-row { display:flex; justify-content:space-between; align-items:center; background:var(--surf); margin-bottom:5px; padding:10px; border-radius:6px; }
        .btns { display:flex; gap:10px; }
        .btn { border:none; padding:8px 12px; border-radius:4px; font-weight:bold; cursor:pointer; }
        .btn-green { background:var(--sec); color:#000; }
        .btn-red { background:var(--warn); color:#000; }
        .btn-blue { background:#3498db; color:#fff; }
        
        /* VIEWER BAR */
        #view-bar { background:#2980b9; padding:8px; text-align:center; display:none; }
        
        .warn-msg { color: var(--warn); font-size: 0.8rem; text-align: center; margin-top: 5px; }
    </style>
</head>
<body>
    <header>
        <div id="app-mode" class="mode-ind mode-live">LIVE MONITOR</div>
        <div id="conn-state" class="conn-stat">OCZEKIWANIE...</div>
    </header>
    
    <!-- FILE VIEWER HEADER (Visible only in Playback) -->
    <div id="view-bar">
        <span>Przeglądasz plik: <b id="view-fname">...</b> </span>
        <button class="btn btn-red" style="padding:2px 8px; margin-left:10px;" onclick="exitViewer()">ZAMKNIJ X</button>
    </div>

    <div class="tabs">
        <button class="tab active" onclick="setTab('dash')">PULPIT</button>
        <button class="tab" onclick="setTab('files')">PLIKI / HISTORIA</button>
    </div>

    <!-- DASHBOARD -->
    <div id="dash" class="page active">
        <!-- Control Panel -->
        <div class="grid">
            <button id="btn-rec" class="btn btn-green full" onclick="toggleRec(true)">START NAGRYWANIA</button>
            <button id="btn-stop" class="btn btn-red full" onclick="toggleRec(false)" disabled>STOP I ZAPISZ</button>
        </div>

        <div id="map-box">
            <div id="map"></div>
            <div id="pause-msg" class="pause-overlay">⏸️ AUTO-PAUZA</div>
        </div>

        <div class="charts-row">
            <div class="chart-wrap"><canvas id="c-speed"></canvas></div>
            <div class="chart-wrap"><canvas id="c-alt"></canvas></div>
        </div>

        <div class="grid">
            <div class="card"><div id="v-speed" class="val">0.0</div><div class="lbl">Prędkość km/h</div></div>
            <div class="card"><div id="v-alt" class="val">0</div><div class="lbl">Wysokość m</div></div>
            <div class="card"><div id="v-dist" class="val">0.00</div><div class="lbl">Dystans km</div></div>
            <div class="card"><div id="v-sats" class="val">0</div><div class="lbl">Satelity</div></div>
        </div>
        <div class="warn-msg" id="gps-warn">Szukanie sygnału GPS...</div>
    </div>

    <!-- FILES -->
    <div id="files" class="page">
        <div class="card full" style="margin-bottom:10px;">
            <button class="btn btn-blue" style="width:100%" onclick="loadList()">ODŚWIEŻ LISTĘ</button>
        </div>
        <div id="file-list">Ładowanie...</div>
    </div>

    <script>
        // GLOBAL STATE
        let map, poly, viewPoly, marker;
        let cSpeed, cAlt;
        let mode = 'LIVE'; // LIVE | VIEW
        let refreshing = false;
        
        // INIT
        window.onload = () => {
            initMap();
            initCharts();
            setInterval(loop, 1500); // Main loop
            loadList(); // Initial load
        };

        function initMap() {
            try {
                map = L.map('map').setView([52.0, 19.0], 6);
                L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png', { maxZoom:19 }).addTo(map);
                poly = L.polyline([], {color:'#bb86fc', weight:4}).addTo(map);
                viewPoly = L.polyline([], {color:'#3498db', weight:4, dashArray:'5,5'}).addTo(map);
                marker = L.circleMarker([0,0], {radius:5, color:'#fff'}).addTo(map);
            } catch(e) { document.getElementById('map-box').innerHTML = '<div style="padding:20px;text-align:center">Brak Internetu - Mapa Offline</div>'; }
        }

        function initCharts() {
            const opts = { responsive:true, maintainAspectRatio:false, scales:{x:{display:false}, y:{grid:{color:'#333'}}}, plugins:{legend:{display:false}} };
            // Speed Chart
            cSpeed = new Chart(document.getElementById('c-speed'), { 
                type:'line', 
                data:{labels:[], datasets:[{data:[], borderColor:'#03dac6', tension:0.1, pointRadius:0, borderWidth:1, fill:true, backgroundColor:'#03dac622'}]}, 
                options:{...opts, plugins:{title:{display:true, text:'PRĘDKOŚĆ'}}} 
            });
            // Altitude Chart
            cAlt = new Chart(document.getElementById('c-alt'), { 
                type:'line', 
                data:{labels:[], datasets:[{data:[], borderColor:'#cf6679', tension:0.1, pointRadius:0, borderWidth:1, fill:true, backgroundColor:'#cf667922'}]}, 
                options:{...opts, plugins:{title:{display:true, text:'WYSOKOŚĆ'}}} 
            });
        }

        function loop() {
            if(mode === 'VIEW') return; // Don't fetch status if viewing file
            
            fetch('/api/status', { signal: AbortSignal.timeout(2000) })
                .then(r => r.json())
                .then(d => {
                    document.getElementById('conn-state').innerText = "POŁĄCZONO";
                    document.getElementById('conn-state').style.color = "#2ecc71";
                    updateDash(d);
                })
                .catch(e => {
                    document.getElementById('conn-state').innerText = "ROZŁĄCZONO"; 
                    document.getElementById('conn-state').style.color = "#cf6679";
                });
        }

        function updateDash(d) {
            // State: 0=IDLE, 1=REC, 2=PAUSE
            const recBtn = document.getElementById('btn-rec');
            const stopBtn = document.getElementById('btn-stop');
            const pauseMsg = document.getElementById('pause-msg');
            const gpsMsg = document.getElementById('gps-warn');
            const appMode = document.getElementById('app-mode');

            if(d.state === 1) { // REC
                recBtn.disabled = true; stopBtn.disabled = false;
                pauseMsg.style.display = 'none';
                appMode.innerText = "NAGRYWANIE"; appMode.className = "mode-ind btn-red";
            } else if(d.state === 2) { // PAUSE
                recBtn.disabled = true; stopBtn.disabled = false;
                pauseMsg.style.display = 'block';
                appMode.innerText = "AUTO-PAUZA"; appMode.className = "mode-ind btn-green"; // Use different color
            } else { // IDLE
                recBtn.disabled = false; stopBtn.disabled = true;
                pauseMsg.style.display = 'none';
                appMode.innerText = "LIVE MONITOR"; appMode.className = "mode-ind mode-live";
            }

            document.getElementById('v-speed').innerText = d.speed.toFixed(1);
            document.getElementById('v-alt').innerText = d.alt.toFixed(0);
            document.getElementById('v-dist').innerText = (d.dist/1000).toFixed(2);
            document.getElementById('v-sats').innerText = d.sats;

            if(d.lat != 0) {
                gpsMsg.style.display = 'none';
                const ll = [d.lat, d.lon];
                marker.setLatLng(ll);
                if(d.state === 1) { // Only update chart/path if recording
                    poly.addLatLng(ll);
                    map.setView(ll);
                    
                    // Update Live Charts
                    pushChart(cSpeed, d.speed);
                    pushChart(cAlt, d.alt);
                }
            } else {
                gpsMsg.style.display = 'block';
            }
        }

        function pushChart(chart, val) {
            if(chart.data.labels.length > 40) { chart.data.labels.shift(); chart.data.datasets[0].data.shift(); }
            chart.data.labels.push('');
            chart.data.datasets[0].data.push(val);
            chart.update('none');
        }

        // CONTROL
        function toggleRec(start) {
            fetch(start ? '/api/start' : '/api/stop').then(() => {
                if(start) { 
                    poly.setLatLngs([]); 
                    cSpeed.data.datasets[0].data = []; cSpeed.update();
                    cAlt.data.datasets[0].data = []; cAlt.update();
                }
                setTimeout(loop, 500);
            });
        }

        // FILES
        function loadList() {
            document.getElementById('file-list').innerHTML = "Pobieranie listy...";
            fetch('/api/files').then(r => r.json()).then(files => {
                let h = '';
                if(files.length === 0) h = '<div style="text-align:center; padding:10px;">Brak plików</div>';
                files.forEach(f => {
                    const fnameEncoded = encodeURIComponent(f.name); // FIX PATHS
                    h += `<div class="file-row">
                        <div><b>${f.name}</b><br><small>${(f.size/1024).toFixed(1)} KB</small></div>
                        <div class="btns">
                            <button class="btn btn-green" onclick="viewFile('${fnameEncoded}')">👁️</button>
                            <a href="/download?file=${fnameEncoded}" class="btn btn-blue" target="_blank">⬇️</a>
                            <button class="btn btn-red" onclick="delFile('${fnameEncoded}')">🗑️</button>
                        </div>
                    </div>`;
                });
                document.getElementById('file-list').innerHTML = h;
            }).catch(e => document.getElementById('file-list').innerHTML = "Błąd listy!");
        }

        function delFile(name) {
            if(confirm("Usunąć?")) {
                fetch('/delete?file=' + name, {method:'DELETE'}).then(loadList);
            }
        }

        // VIEWER
        function viewFile(name) {
            mode = 'VIEW';
            document.getElementById('view-bar').style.display = 'block';
            document.getElementById('view-fname').innerText = decodeURIComponent(name);
            document.getElementById('app-mode').innerText = "TRYB PRZEGLĄDANIA";
            document.getElementById('app-mode').className = "mode-ind mode-view";
            
            setTab('dash'); // Jump to map to see view
            
            fetch('/download?file=' + name).then(r => r.text()).then(csv => {
                const lines = csv.split('\n');
                const path = [];
                
                // Reset Charts for View
                cSpeed.data.labels = []; cSpeed.data.datasets[0].data = [];
                cAlt.data.labels = []; cAlt.data.datasets[0].data = [];

                lines.forEach(l => {
                   const p = l.split(',');
                   if(p.length > 5 && !isNaN(p[1])) {
                       const lat = parseFloat(p[1]);
                       const lon = parseFloat(p[2]);
                       if(lat!=0) {
                           path.push([lat, lon]);
                           cSpeed.data.datasets[0].data.push(parseFloat(p[3]));
                           cSpeed.data.labels.push('');
                           cAlt.data.datasets[0].data.push(parseFloat(p[4]));
                           cAlt.data.labels.push('');
                       }
                   }
                });

                viewPoly.setLatLngs(path);
                cSpeed.update();
                cAlt.update();
                if(path.length) map.fitBounds(viewPoly.getBounds());
            });
        }

        function exitViewer() {
            mode = 'LIVE';
            document.getElementById('view-bar').style.display = 'none';
            viewPoly.setLatLngs([]);
            cSpeed.data.datasets[0].data = []; cSpeed.update(); // Clear view data
            cAlt.data.datasets[0].data = []; cAlt.update();
            loadList(); // Refresh list to get real buttons back if needed
            setTab('dash');
        }

        function setTab(t) {
            document.querySelectorAll('.page').forEach(e=>e.classList.remove('active'));
            document.getElementById(t).classList.add('active');
            document.querySelectorAll('.tab').forEach(e=>e.classList.remove('active'));
            event.target.classList.add('active');
            if(t==='dash' && map) setTimeout(()=>map.invalidateSize(), 200);
        }
    </script>
</body>
</html>
)rawliteral";

#endif
