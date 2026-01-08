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
        header { background:var(--surf); padding:10px; display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid #333; z-index:10; flex-shrink: 0; }
        .mode-ind { font-weight:bold; font-size:0.8rem; padding:4px 8px; border-radius:4px; }
        .mode-live { background:#2ecc71; color:#000; }
        .mode-view { background:#3498db; color:#fff; }
        .conn-stat { font-size:0.7rem; color:#888; }

        /* TABS */
        .tabs { display:flex; background:#000; flex-shrink: 0; }
        .tab { flex:1; padding:12px; background:none; border:none; color:#777; font-weight:bold; border-bottom:3px solid transparent; }
        .tab.active { color:var(--sec); border-bottom-color:var(--sec); }

        /* CONTENT */
        .page { flex:1; overflow-y:auto; padding:10px; display:none; -webkit-overflow-scrolling: touch; padding-bottom: 40px; }
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

        /* CHARTS - stacked vertically */
        .charts-container { display:flex; flex-direction:column; gap:10px; margin-bottom:10px; }
        .chart-wrap { background:var(--surf); border-radius:8px; padding:2px; position:relative; height:150px; width:100%; }
        
        /* FILES */
        .file-row { display:flex; flex-direction:column; background:var(--surf); margin-bottom:5px; padding:10px; border-radius:6px; gap:8px; }
        .file-info { font-weight:bold; }
        .btns { display:flex; gap:10px; width:100%; justify-content:space-between; }
        
        /* Outline Buttons */
        .btn { border:1px solid transparent; padding:8px 6px; border-radius:4px; font-weight:bold; cursor:pointer; background:transparent; font-size:0.75rem; flex:1; text-align:center; text-decoration:none; display:inline-block; }
        .btn-green { color:var(--sec); border-color:var(--sec); }
        .btn-red { color:var(--warn); border-color:var(--warn); }
        .btn-blue { color:#3498db; border-color:#3498db; }
        
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
        <div id="pnl-idle" class="grid">
            <button class="btn btn-green full" onclick="req('/api/start')">START</button>            <button class="btn btn-blue full" onclick="req('/api/sleep')">UŚPIJ (Deep Sleep)</button>        </div>
        <div id="pnl-rec" class="grid" style="display:none;">
            <button class="btn btn-blue full" onclick="req('/api/pause')">PAUZA</button>
        </div>
        <div id="pnl-paused" class="grid" style="display:none;">
            <button class="btn btn-green full" onclick="req('/api/start')" style="margin-bottom:5px">WZNÓW</button>
            <button class="btn btn-blue" onclick="req('/api/stop')">ZAPISZ</button>
            <button class="btn btn-red" onclick="req('/api/discard')">ODRZUĆ</button>
        </div>

        <div id="map-box">
            <div id="map"></div>
            <div id="pause-msg" class="pause-overlay">⏸️ PAUZA / AUTO-PAUZA</div>
        </div>

        <div class="charts-container">
            <div class="chart-wrap"><canvas id="c-speed"></canvas></div>
            <div class="chart-wrap"><canvas id="c-alt"></canvas></div>
            <div class="chart-wrap"><canvas id="c-hdop"></canvas></div>
        </div>

        <div class="grid">
            <div class="card"><div id="v-speed" class="val">0.0</div><div class="lbl">Prędkość km/h</div></div>
            <div class="card"><div id="v-alt" class="val">0</div><div class="lbl">Wysokość m</div></div>
            <div class="card"><div id="v-dist" class="val">0.00</div><div class="lbl">Dystans km</div></div>
            <div class="card"><div id="v-sats" class="val">0</div><div class="lbl">Satelity</div></div>
            <div class="card"><div id="v-hdop" class="val">-</div><div class="lbl">HDOP</div></div>
            <div class="card"><div id="v-batt" class="val">-</div><div class="lbl">Bateria V</div></div>
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
        let cSpeed, cAlt, cHdop;
        let mode = 'LIVE'; // LIVE | VIEW
        let refreshing = false;
        let startMarker, endMarker; // Markers for file view
        
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
                // ESRI Satellite Map - much better for GPS tracking
                L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', { 
                    attribution: 'Tiles &copy; Esri',
                    maxZoom:19 
                }).addTo(map);
                poly = L.polyline([], {color:'#bb86fc', weight:4}).addTo(map);
                viewPoly = L.polyline([], {color:'#3498db', weight:4, dashArray:'5,5'}).addTo(map);
                marker = L.circleMarker([0,0], {radius:5, color:'#fff'}).addTo(map);
            } catch(e) { document.getElementById('map-box').innerHTML = '<div style="padding:20px;text-align:center">Brak Internetu - Mapa Offline</div>'; }
        }

        function initCharts() {
            // FIXED: Y-axis precision to 1, Time axis labels added, Auto-scaling fixed (suggestedMin:0)
            const opts = { 
                responsive:true, 
                maintainAspectRatio:false, 
                animation: false,
                scales:{
                    x:{ 
                        display:true, 
                        grid:{display:false},
                        ticks:{ 
                            color:'#777', 
                            maxTicksLimit: 6,
                            font:{size:9}
                        } 
                    }, 
                    y:{
                        grid:{color:'#333'},
                        ticks:{ color:'#bbb', precision:0 },
                        suggestedMin: 0
                    }
                }, 
                plugins:{
                    legend:{display:false}, // REMOVED undefined legend
                    tooltip:{enabled:true, intersect:false, mode:'index'}
                } 
            };
            
            // Speed Chart
            cSpeed = new Chart(document.getElementById('c-speed'), { 
                type:'line', 
                data:{labels:[], datasets:[{label:'km/h', data:[], borderColor:'#03dac6', tension:0.1, pointRadius:0, borderWidth:2, fill:true, backgroundColor:'#03dac622'}]}, 
                options:{...opts, plugins:{...opts.plugins, title:{display:true, text:'PRĘDKOŚĆ (km/h)', color:'#eee', font:{size:10}}}} 
            });
            // Altitude Chart
            cAlt = new Chart(document.getElementById('c-alt'), { 
                type:'line', 
                data:{labels:[], datasets:[{label:'m', data:[], borderColor:'#cf6679', tension:0.1, pointRadius:0, borderWidth:2, fill:true, backgroundColor:'#cf667922'}]}, 
                options:{...opts, plugins:{...opts.plugins, title:{display:true, text:'WYSOKOŚĆ (m)', color:'#eee', font:{size:10}}}} 
            });

            // HDOP Chart
            cHdop = new Chart(document.getElementById('c-hdop'), { 
                type:'line', 
                data:{labels:[], datasets:[{label:'HDOP', data:[], borderColor:'#bb86fc', tension:0.1, pointRadius:0, borderWidth:2, fill:true, backgroundColor:'#bb86fc22'}]}, 
                options:{...opts, plugins:{...opts.plugins, title:{display:true, text:'PRECYZJA GPS (HDOP)', color:'#eee', font:{size:10}}}} 
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
            const pnlIdle = document.getElementById('pnl-idle');
            const pnlRec = document.getElementById('pnl-rec');
            const pnlPaused = document.getElementById('pnl-paused');
            
            const pauseMsg = document.getElementById('pause-msg');
            const gpsMsg = document.getElementById('gps-warn');
            const appMode = document.getElementById('app-mode');

            // Hide all panels first
            pnlIdle.style.display = 'none';
            pnlRec.style.display = 'none';
            pnlPaused.style.display = 'none';

            if(d.state === 1) { // REC
                pnlRec.style.display = 'grid';
                pauseMsg.style.display = 'none';
                appMode.innerText = "NAGRYWANIE"; appMode.className = "mode-ind btn-red";
            } else if(d.state === 2) { // PAUSE
                pnlPaused.style.display = 'grid';
                pauseMsg.style.display = 'block';
                // Adjust text for auto-pause vs manual pause logic if needed, but for now just Pause
                appMode.innerText = "PAUZA"; appMode.className = "mode-ind btn-green";
            } else { // IDLE
                pnlIdle.style.display = 'grid';
                pauseMsg.style.display = 'none';
                appMode.innerText = "GOTOWY"; appMode.className = "mode-ind mode-live";
            }

            document.getElementById('v-speed').innerText = d.speed.toFixed(1);
            document.getElementById('v-alt').innerText = d.alt.toFixed(0);
            document.getElementById('v-dist').innerText = (d.dist/1000).toFixed(2);
            document.getElementById('v-sats').innerText = d.sats;
            document.getElementById('v-hdop').innerText = d.hdop.toFixed(1);
            document.getElementById('v-batt').innerText = d.batt.toFixed(2);

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
                    pushChart(cHdop, d.hdop);
                }
            } else {
                gpsMsg.style.display = 'block';
            }
        }

        function pushChart(chart, val) {
            // Keep last 60 points (approx 1 minute or more depending on updates)
            if(chart.data.labels.length > 60) { chart.data.labels.shift(); chart.data.datasets[0].data.shift(); }
            
            // Add simple time label (MM:SS)
            let d = new Date();
            let label = d.getMinutes().toString().padStart(2,'0') + ':' + d.getSeconds().toString().padStart(2,'0');
            
            chart.data.labels.push(label);
            chart.data.datasets[0].data.push(val);
            chart.update('none'); // Optimized update mode
        }

        // CONTROL
        function req(url) {
            fetch(url).then(() => {
                // Clear data on fresh start
                if(url.includes('start') && document.getElementById('pnl-idle').style.display !== 'none') {
                    poly.setLatLngs([]); 
                    cSpeed.data.datasets[0].data = []; cSpeed.update();
                    cAlt.data.datasets[0].data = []; cAlt.update();
                    cHdop.data.datasets[0].data = []; cHdop.update();
                }
                setTimeout(loop, 200);
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
                        <div class="file-info">${f.name} <small style="color:#888;">(${(f.size/1024).toFixed(1)} KB)</small></div>
                        <div class="btns">
                            <button class="btn btn-green" onclick="viewFile('${fnameEncoded}')">PODGLĄD</button>
                            <a href="/download?file=${fnameEncoded}" class="btn btn-blue" target="_blank" download>POBIERZ</a>
                            <button class="btn btn-red" onclick="delFile('${fnameEncoded}')">USUŃ</button>
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
                
                // Remove old start/end markers
                if(startMarker) map.removeLayer(startMarker);
                if(endMarker) map.removeLayer(endMarker);

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

                // Add start marker (green) and end marker (red)
                if(path.length > 0) {
                    startMarker = L.circleMarker(path[0], {radius:8, color:'#2ecc71', fill:true, fillColor:'#2ecc71', fillOpacity:0.8, weight:2}).addTo(map);
                    endMarker = L.circleMarker(path[path.length-1], {radius:8, color:'#cf6679', fill:true, fillColor:'#cf6679', fillOpacity:0.8, weight:2}).addTo(map);
                }

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
            if(startMarker) map.removeLayer(startMarker);
            if(endMarker) map.removeLayer(endMarker);
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
