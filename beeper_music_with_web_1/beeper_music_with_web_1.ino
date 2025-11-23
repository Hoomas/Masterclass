// ================= ESP32 Web Piano Roll (STA, 2 cores) =================
// Core 0: веб-сервер
// Core 1: плеер (playerTick), чтобы звук не лагал, пока сервер шлёт HTML.
// HTML отдаём из PROGMEM одним куском, чтобы не тормозить на println.

// ====== Wi-Fi ======
#include <WiFi.h>
#include <math.h>

const char* WIFI_SSID = "Semen_Public";     // ← впиши свою сеть
const char* WIFI_PASS = "protocol_low_speed";   // ← впиши свой пароль

// ====== pins ======
#define BEEP_PIN 15//7
#define LED_PIN  35

WiFiServer server(80);

// ====== player data ======
struct Event {
  int      freq;
  uint32_t startMs;
  uint32_t durMs;
};

const int MAX_EVENTS = 256;
Event     eventsBuf[MAX_EVENTS];
int       eventsLen      = 0;
bool      playing        = false;
uint32_t  playStartMs    = 0;
int       currentIdx     = 0;
uint32_t  currentNoteEnd = 0;
int       currentFreq    = 0;
uint32_t  totalTrackMs   = 0;

const uint32_t MIN_NOTE_MS = 60;   // одиночные ноты не исчезают

// ====== HTML (PROGMEM) ======
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Piano Roll</title>
<style>
body{margin:0;background:#0d0f12;color:#e5e7eb;font-family:system-ui,Arial;overflow:hidden}
.bar{display:flex;gap:10px;align-items:center;flex-wrap:wrap;background:#111827;padding:8px 10px;position:sticky;top:0;z-index:10}
input,button{background:#1f2937;color:#e5e7eb;border:1px solid #374151;border-radius:5px;padding:4px 8px}
button{cursor:pointer}
#wrap{padding:8px;height:calc(100vh - 60px)}
#roll{background:#111317;border:1px solid #374151;display:block;touch-action:none;width:100%;height:100%}
#prog{width:240px}
.on{background:#22c55e !important}
</style>
</head>
<body>
<div class="bar">
<label>BPM <input id="bpm" type="number" min="30" max="240" value="120"></label>
<button onclick="play()">Play</button>
<button onclick="stopPlay()">Stop</button>
<button id="loopBtn" onclick="toggleLoop()">Loop: OFF</button>
<button id="eraseBtn" onclick="toggleErase()">Eraser: OFF</button>
<label style="font-size:12px">MIDI <input id="midiFile" type="file" accept=".mid,.midi" onchange="loadMidi(event)" style="font-size:12px"></label>
<input id="prog" type="range" min="0" max="1" value="0" step="1" disabled>
<span style="font-size:12px;opacity:.8">Ноты слева, поле справа. Клик=нота. Eraser=стереть. MIDI: загрузить файл.</span>
</div>
<div id="wrap">
<canvas id="roll"></canvas>
</div>
<script>
"use strict";
let canvas,ctx,W,H;
const rows=24,cols=128;
let cellW=22,cellH=22;
const labelW=70;
let areaW=cols*cellW;
let bpm=120;
const notes=[];
const baseMidi=48;
let drag=null;
let isPlaying=false,playStartT=0,totalMs=0,rafId=0,statusTimer=null,loopEnabled=false;
let eraseMode=false;
const defaultTempoUs=500000;

function calcTotalMs(){
  const qMs=Math.floor(60000/Math.max(30,Math.min(240,bpm)));
  const cellMs=Math.floor(qMs/4);
  totalMs=cols*cellMs;
  const p=document.getElementById("prog");
  p.max=totalMs>0?totalMs:1;
  p.value=0;
}

function resize(){
  const avail=Math.max(window.innerWidth-16, labelW+200);
  const desired=avail-labelW;
  const minCell=12, maxCell=26;
  cellW=Math.max(minCell, Math.min(maxCell, Math.floor(desired/cols)));
  areaW=cellW*cols;
  canvas.width=labelW+areaW;
  canvas.height=rows*cellH;
  W=canvas.width; H=canvas.height;
  draw();
}

function drawGrid(){
  ctx.fillStyle="#0b0f14"; ctx.fillRect(0,0,labelW,H);
  ctx.strokeStyle="#475569"; ctx.beginPath(); ctx.moveTo(labelW+0.5,0); ctx.lineTo(labelW+0.5,H); ctx.stroke();
  ctx.fillStyle="#111317"; ctx.fillRect(labelW,0,areaW,H);
  for(let c=0;c<=cols;c++){
    const x=labelW+c*cellW+0.5;
    ctx.strokeStyle=(c%4===0)?"#475569":"#2d3748";
    ctx.beginPath(); ctx.moveTo(x,0); ctx.lineTo(x,H); ctx.stroke();
  }
  for(let r=0;r<=rows;r++){
    const y=H-r*cellH-0.5;
    ctx.strokeStyle="#2d3748";
    ctx.beginPath(); ctx.moveTo(labelW,y); ctx.lineTo(labelW+areaW,y); ctx.stroke();
  }
  ctx.textBaseline="middle"; ctx.font="12px system-ui,Arial";
  const names=["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"];
  for(let r=0;r<rows;r++){
    const midi=baseMidi+r;
    const y=H-(r+0.5)*cellH;
    const pc=midi%12;
    const isBlack=[1,3,6,8,10].includes(pc);
    if(isBlack){ ctx.fillStyle="rgba(255,255,255,0.035)"; ctx.fillRect(labelW,H-(r+1)*cellH,areaW,cellH); }
    const oct=Math.floor(midi/12)-1;
    ctx.fillStyle=(pc===0)?"#7dd3fc":(isBlack?"#cbd5e1":"#e5e7eb");
    ctx.fillText(names[pc]+oct,8,y);
  }
}

function drawNotes(){
  for(const n of notes){
    const x=labelW+n.col*cellW+1, y=H-(n.row+1)*cellH+1;
    const w=Math.max(4,n.len*cellW-2), h=cellH-2;
    ctx.fillStyle="#38bdf8"; ctx.fillRect(x,y,w,h);
    ctx.strokeStyle="#0ea5e9"; ctx.lineWidth=1; ctx.strokeRect(x+0.5,y+0.5,w-1,h-1);
  }
}

function drawPlayhead(){
  if(!isPlaying||totalMs<=0) return;
  let t=performance.now()-playStartT;
  if(t<0) t=0;
  if(t>totalMs) t=totalMs;
  document.getElementById("prog").value=Math.floor(t);
  const x=labelW+(t/totalMs)*areaW+0.5;
  ctx.strokeStyle="#ef4444"; ctx.lineWidth=2;
  ctx.beginPath(); ctx.moveTo(x,0); ctx.lineTo(x,H); ctx.stroke();
}

function draw(){ drawGrid(); drawNotes(); drawPlayhead(); }

function posToCell(px,py){
  if(px<labelW) return {row:-1,col:-1};
  px-=labelW;
  let col=Math.floor(px/cellW), row=Math.floor((H-1-py)/cellH);
  if(col<0)col=0; if(col>cols-1)col=cols-1;
  if(row<0)row=0; if(row>rows-1)row=rows-1;
  return {row,col};
}

function noteAt(col,row){
  for(let i=notes.length-1;i>=0;i--){
    const n=notes[i];
    if(row===n.row && col>=n.col && col<=n.col+n.len-1) return i;
  }
  return -1;
}

function toggleErase(){
  eraseMode=!eraseMode;
  const b=document.getElementById("eraseBtn");
  if(eraseMode){ b.classList.add("on"); b.textContent="Eraser: ON"; }
  else{ b.classList.remove("on"); b.textContent="Eraser: OFF"; }
}

function toggleLoop(){
  loopEnabled=!loopEnabled;
  const b=document.getElementById("loopBtn");
  if(loopEnabled){ b.classList.add("on"); b.textContent="Loop: ON"; }
  else{ b.classList.remove("on"); b.textContent="Loop: OFF"; }
}

function onDown(e){
  const r=canvas.getBoundingClientRect();
  const x=e.clientX-r.left, y=e.clientY-r.top;
  const p=posToCell(x,y);
  if(p.row<0) return;
  const idx=noteAt(p.col,p.row);
  if(eraseMode){
    if(idx>=0){ notes.splice(idx,1); draw(); }
    return;
  }
  if(idx>=0){
    const n=notes[idx], tail=n.col+n.len-1;
    if(p.col>=tail) drag={type:"resize",idx};
    else drag={type:"move",idx,dx:p.col-n.col};
  }else{
    notes.push({row:p.row,col:p.col,len:1});
    drag={type:"new",idx:notes.length-1,startX:p.col};
  }
  draw();
}

function onMove(e){
  if(!drag || eraseMode) return;
  const r=canvas.getBoundingClientRect();
  let x=e.clientX-r.left, y=e.clientY-r.top;
  if(x<labelW) x=labelW;
  if(x>labelW+areaW-1) x=labelW+areaW-1;
  const p=posToCell(x,y);
  if(p.row<0) return;
  const n=notes[drag.idx];
  if(drag.type==="new"){
    let len=p.col-drag.startX+1; if(len<1)len=1;
    n.row=p.row; n.col=(p.col<drag.startX)?p.col:drag.startX; n.len=len;
  }else if(drag.type==="resize"){
    let len=p.col-n.col+1; if(len<1)len=1; const maxLen=cols-n.col; if(len>maxLen)len=maxLen; n.len=len;
  }else{
    let newCol=p.col-drag.dx; if(newCol<0)newCol=0; if(newCol>cols-n.len)newCol=cols-n.len; n.col=newCol; n.row=p.row;
  }
  draw();
}

function onUp(){ drag=null; }

function setBPM(){
  let v=parseInt(document.getElementById("bpm").value||"120");
  if(v<30)v=30; if(v>240)v=240;
  bpm=v; calcTotalMs(); draw();
}

function startStatusPoll(){
  if(statusTimer) clearInterval(statusTimer);
  statusTimer=setInterval(async()=>{
    try{
      const r=await fetch("/status");
      const st=await r.json();
      const qMs=Math.floor(60000/Math.max(30,Math.min(240,bpm)));
      const cellMs=Math.floor(qMs/4);
      const gridTotal=cols*cellMs;
      if(st.playing){
        let el=st.elapsed; if(el>gridTotal) el=gridTotal;
        playStartT=performance.now()-el;
        totalMs=gridTotal;
        document.getElementById("prog").max=gridTotal;
        isPlaying=true;
      }else{
        if(isPlaying && loopEnabled && notes.length>0){
          await play(true);
        }else{
          isPlaying=false;
        }
      }
    }catch(e){}
  },200);
}

function stopStatusPoll(){
  if(statusTimer){ clearInterval(statusTimer); statusTimer=null; }
}

async function play(skipScroll=false){
  setBPM();
  const qMs=Math.floor(60000/bpm);
  const cellMs=Math.floor(qMs/4);
  let body="";
  for(const n of notes){
    const midi=baseMidi+n.row;
    const startMs=n.col*cellMs;
    const durMs=n.len*cellMs;
    body+=midi+","+startMs+","+durMs+";";
  }
  try{
    await fetch("/play",{method:"POST",headers:{"Content-Type":"text/plain"},body});
    isPlaying=true;
    playStartT=performance.now();
    startStatusPoll();
    if(!skipScroll){
      const prog=document.getElementById("prog");
      prog.max=totalMs;
      prog.value=0;
    }
    if(rafId) cancelAnimationFrame(rafId);
    (function loop(){ draw(); if(isPlaying) rafId=requestAnimationFrame(loop); })();
  }catch(e){}
}

async function stopPlay(){
  isPlaying=false;
  stopStatusPoll();
  if(rafId) cancelAnimationFrame(rafId);
  document.getElementById("prog").value=0;
  try{ await fetch("/stop"); }catch(e){}
  draw();
}

function readVarLen(data,idx){
  let val=0,b;
  do{ b=data[idx++]; val=(val<<7)|(b&0x7f); }while(b&0x80 && idx<data.length);
  return {val,next:idx};
}

function parseMidiFile(buf){
  const data=new Uint8Array(buf);
  let i=0;
  if(String.fromCharCode(...data.slice(0,4))!="MThd") throw new Error("No header");
  i=8; // skip chunk len
  const division=(data[12]<<8)|data[13];
  const ticksPerQ=division&0x7fff;
  i=14;
  if(ticksPerQ<=0) throw new Error("Bad division");

  if(String.fromCharCode(...data.slice(i,i+4))!="MTrk") throw new Error("No track");
  const trackLen=(data[i+4]<<24)|(data[i+5]<<16)|(data[i+6]<<8)|data[i+7];
  i+=8;
  const end=i+trackLen;

  let tempoUs=defaultTempoUs;
  const openNotes={};
  const parsed=[];
  let running=0;
  let tTicks=0;

  while(i<end){
    const d=readVarLen(data,i); i=d.next; tTicks+=d.val;
    let status=data[i];
    if(status<0x80){
      if(!running) throw new Error("Missing status");
      status=running;
    }else{ i++; running=status; }

    if(status===0xff){
      const type=data[i++];
      const len=readVarLen(data,i); i=len.next; const metaLen=len.val;
      if(type===0x51 && metaLen===3){
        tempoUs=(data[i]<<16)|(data[i+1]<<8)|data[i+2];
      }
      i+=metaLen;
      continue;
    }

    const evt=status>>4; const ch=status&0x0f;
    if(evt===0x9 || evt===0x8){
      const note=data[i++]; const vel=data[i++];
      const key=`${ch}:${note}`;
      if(evt===0x9 && vel>0){
        const ms=Math.floor((tTicks*tempoUs)/(ticksPerQ*1000));
        openNotes[key]=ms;
      }else{
        if(openNotes[key]!==undefined){
          const startMs=openNotes[key];
          const ms=Math.floor((tTicks*tempoUs)/(ticksPerQ*1000));
          const dur=ms-startMs; if(dur>0) parsed.push({note,startMs,dur});
          delete openNotes[key];
        }
      }
    }else{
      // skip other channel event data lengths
      const skip=[2,2,2,2,1,1,2][evt-0x8]||0;
      i+=skip;
    }
  }

  return {notes:parsed,tempoUs};
}

function mapMidiToGrid(parsed){
  const {notes:parsedNotes,tempoUs}=parsed;
  if(!parsedNotes.length) throw new Error("Нет нот в файле");
  const midiBpm=Math.round(60000000/tempoUs);
  bpm=Math.max(30,Math.min(240,midiBpm));
  document.getElementById("bpm").value=bpm;
  calcTotalMs();
  const qMs=Math.floor(60000/bpm);
  const cellMs=Math.floor(qMs/4);
  notes.length=0;
  for(const n of parsedNotes){
    const row=n.note-baseMidi; if(row<0||row>=rows) continue;
    let col=Math.round(n.startMs/cellMs); if(col>=cols) continue;
    let len=Math.max(1,Math.round(n.dur/cellMs));
    if(col+len>cols) len=cols-col;
    notes.push({row,col,len});
  }
  draw();
}

async function loadMidi(ev){
  const f=ev.target.files[0]; if(!f) return;
  try{
    const buf=await f.arrayBuffer();
    const parsed=parseMidiFile(buf);
    mapMidiToGrid(parsed);
  }catch(e){
    alert("Ошибка MIDI: "+e.message);
  }
}

window.addEventListener("load",()=>{
  canvas=document.getElementById("roll");
  ctx=canvas.getContext("2d");
  document.getElementById("bpm").value=bpm;
  calcTotalMs();
  resize();
  window.addEventListener("resize",resize);
  canvas.addEventListener("mousedown",onDown);
  window.addEventListener("mousemove",onMove);
  window.addEventListener("mouseup",onUp);
  draw();
});
</script>
</body>
</html>
)rawliteral";

// ====== утилиты ======
static inline int midiToFreq(int midi) {
  if (midi <= 0) return 0;
  double f = 440.0 * pow(2.0, ((double)midi - 69.0) / 12.0);
  int fi = (int)lround(f);
  return (fi < 20) ? 0 : fi;
}

// ====== плеер ======
void playerTick() {
  if (!playing) return;
  uint32_t now = millis();

  if (currentFreq != 0) {
    if ((int32_t)(now - currentNoteEnd) >= 0) {
      noTone(BEEP_PIN);
      digitalWrite(LED_PIN, LOW);
      currentFreq = 0;
    }
  }

  while (currentIdx < eventsLen) {
    uint32_t targetStart = playStartMs + eventsBuf[currentIdx].startMs;
    if ((int32_t)(now - targetStart) < 0) break;

    int      f   = eventsBuf[currentIdx].freq;
    uint32_t dur = eventsBuf[currentIdx].durMs;

    if (f > 0) {
      tone(BEEP_PIN, f);
      digitalWrite(LED_PIN, HIGH);
      currentFreq    = f;
      currentNoteEnd = now + dur;
    } else {
      currentFreq    = 0;
      currentNoteEnd = now + dur;
    }

    currentIdx++;
  }

  if (currentIdx >= eventsLen && currentFreq == 0) {
    playing = false;
  }
}

void stopPlay() {
  playing = false;
  noTone(BEEP_PIN);
  digitalWrite(LED_PIN, LOW);
  currentFreq = 0;
  currentIdx = 0;
  currentNoteEnd = 0;
}

// ====== парсинг ======
bool parseBodyIntoEvents(const String& body) {
  eventsLen    = 0;
  totalTrackMs = 0;
  int start = 0;
  while (start < body.length() && eventsLen < MAX_EVENTS) {
    int sep = body.indexOf(';', start);
    if (sep < 0) sep = body.length();
    String tok = body.substring(start, sep);
    tok.trim();
    if (tok.length() > 0) {
      int c1 = tok.indexOf(',');
      int c2 = tok.indexOf(',', c1 + 1);
      if (c1 > 0 && c2 > c1) {
        int midi  = tok.substring(0, c1).toInt();
        int st_i  = tok.substring(c1 + 1, c2).toInt();
        int dur_i = tok.substring(c2 + 1).toInt();
        if (st_i  < 0) st_i  = 0;
        if (dur_i < 1) dur_i = 1;
        if ((uint32_t)dur_i < MIN_NOTE_MS) dur_i = (int)MIN_NOTE_MS;

        eventsBuf[eventsLen].freq    = midiToFreq(midi);
        eventsBuf[eventsLen].startMs = (uint32_t)st_i;
        eventsBuf[eventsLen].durMs   = (uint32_t)dur_i;

        uint32_t endMs = (uint32_t)st_i + (uint32_t)dur_i;
        if (endMs > totalTrackMs) totalTrackMs = endMs;

        eventsLen++;
      }
    }
    start = sep + 1;
  }
  return eventsLen > 0;
}

// ====== чтение запроса (использует серверная таска) ======
bool readRequest(WiFiClient& client, String& method, String& path, int& contentLen, String& body) {
  method = ""; path = ""; contentLen = 0; body = "";
  String reqLine = "";
  unsigned long t0 = millis();

  while (client.connected() && (millis() - t0) < 3000) {
    if (client.available()) {
      char c = client.read();
      if (c == '\r') continue;
      if (c == '\n') break;
      reqLine += c;
    } else {
      vTaskDelay(1);
    }
  }
  if (reqLine.length() == 0) return false;

  int sp1 = reqLine.indexOf(' ');
  int sp2 = reqLine.indexOf(' ', sp1 + 1);
  if (sp1 < 0) return false;
  method = reqLine.substring(0, sp1);
  path   = (sp2 > 0) ? reqLine.substring(sp1 + 1, sp2) : reqLine.substring(sp1 + 1);

  while (client.connected() && (millis() - t0) < 3000) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
    int p = line.indexOf("Content-Length:");
    if (p >= 0) {
      String num = line.substring(p + 15);
      num.trim();
      contentLen = num.toInt();
    }
  }

  if (method == "POST" && contentLen > 0) {
    body.reserve(contentLen);
    int readBytes = 0;
    while (client.connected() && readBytes < contentLen && (millis() - t0) < 4000) {
      if (client.available()) {
        char c = client.read();
        body += c;
        readBytes++;
      } else {
        vTaskDelay(1);
      }
    }
  }

  return true;
}

// ====== SERVER TASK (core 0) ======
void serverTask(void* pv) {
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    WiFiClient client = server.available();
    if (!client) {
      vTaskDelay(5 / portTICK_PERIOD_MS);
      continue;
    }

    String method, path, body;
    int contentLen = 0;
    if (!readRequest(client, method, path, contentLen, body)) {
      client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"));
      client.stop();
      continue;
    }

    if (method == "GET" && (path == "/" || path.startsWith("/index"))) {
      client.println(F("HTTP/1.1 200 OK"));
      client.println(F("Content-Type: text/html; charset=UTF-8"));
      client.println(F("Connection: close"));
      client.println();
      client.write((const uint8_t*)INDEX_HTML, strlen(INDEX_HTML));
    }
    else if (method == "POST" && path.startsWith("/play")) {
      bool ok = parseBodyIntoEvents(body);
      if (ok) {
        playing     = true;
        playStartMs = millis();
        currentIdx  = 0;
        currentFreq = 0;
        client.println(F("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nPlaying"));
      } else {
        stopPlay();
        client.println(F("HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nNo events"));
      }
    }
    else if (method == "GET" && path.startsWith("/stop")) {
      stopPlay();
      client.println(F("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nStopped"));
    }
    else if (method == "GET" && path.startsWith("/status")) {
      uint32_t elapsed = playing ? (millis() - playStartMs) : 0;
      client.println(F("HTTP/1.1 200 OK"));
      client.println(F("Content-Type: application/json"));
      client.println(F("Connection: close"));
      client.println();
      client.print(F("{\"playing\":"));
      client.print(playing ? F("true") : F("false"));
      client.print(F(",\"elapsed\":"));
      client.print(elapsed);
      client.print(F(",\"total\":"));
      client.print(totalTrackMs);
      client.println(F("}"));
    }
    else {
      client.println(F("HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nNot Found"));
    }

    client.stop();
    // отдаём ядро чуть-чуть
    vTaskDelay(1);
  }
}

// ====== PLAYER TASK (core 1) ======
void playerTask(void* pv) {
  for (;;) {
    playerTick();
    vTaskDelay(1 / portTICK_PERIOD_MS);  // 1 мс достаточно
  }
}

// ====== SETUP / LOOP ======
void setup() {
  Serial.begin(115200);
  pinMode(BEEP_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  noTone(BEEP_PIN);
  digitalWrite(LED_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // чтобы не залипал при больших ответах
  Serial.print("Connecting to "); Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint8_t tries = 40;
  while (WiFi.status() != WL_CONNECTED && tries--) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected.");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    server.begin();
  } else {
    Serial.println("WiFi not connected. Check SSID/PASS.");
  }

  // создаём две таски
  xTaskCreatePinnedToCore(serverTask, "serverTask", 8192, NULL, 1, NULL, 0); // core 0
  xTaskCreatePinnedToCore(playerTask, "playerTask", 4096, NULL, 1, NULL, 1); // core 1
}

void loop() {
  // всё уже в тасках
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}
