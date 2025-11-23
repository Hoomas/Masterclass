// ======================= ESP32 Web Piano Roll (STA, labels LEFT) =======================
// Подключается к твоему Wi-Fi, НЕ поднимает свою точку доступа.
// В браузере открываешь IP, который ESP32 выведет в Serial.
// Веб-интерфейс: ноты слева, сетка справа, таймлайн по всей длине.
// Маршруты:
//   GET  /         -> страница
//   POST /play     -> "midi,startMs,durMs;..."
//   GET  /stop
//   GET  /status   -> {"playing":true/false,"elapsed":...,"total":...}

#include <WiFi.h>
#include <math.h>

// ------------- ВПИШИ СВОИ ДАННЫЕ -------------
const char* WIFI_SSID = "BKZ_MKZ";     // ← твоя сеть
const char* WIFI_PASS = "cudo_BKZ_MKZ";   // ← твой пароль

// ------------- ПИНЫ -------------
#define BEEP_PIN 7
#define LED_PIN  35

// ------------- СЕРВЕР -------------
WiFiServer server(80);

// ------------- ПЛЕЕР -------------
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
uint32_t  totalTrackMs   = 0;   // по факту нот (ESP всё равно знает)

static inline int midiToFreq(int midi) {
  if (midi <= 0) return 0;
  double f = 440.0 * pow(2.0, ((double)midi - 69.0) / 12.0);
  int fi = (int)lround(f);
  return (fi < 20) ? 0 : fi;
}

void stopPlay() {
  playing = false;
  noTone(BEEP_PIN);
  digitalWrite(LED_PIN, LOW);
  currentFreq = 0;
  currentIdx = 0;
  currentNoteEnd = 0;
}

void startPlay() {
  if (eventsLen <= 0) return;
  playing     = true;
  playStartMs = millis();
  currentIdx  = 0;
  currentFreq = 0;
  currentNoteEnd = 0;
}

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
    stopPlay();
  }
}

// "midi,startMs,durMs;..."
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

// читаем запрос
bool readRequest(WiFiClient& client, String& method, String& path, int& contentLen, String& body) {
  method = "";
  path = "";
  contentLen = 0;
  body = "";

  String reqLine = "";
  unsigned long t0 = millis();
  while (client.connected() && (millis() - t0) < 3000) {
    if (client.available()) {
      char c = client.read();
      if (c == '\r') continue;
      if (c == '\n') break;
      reqLine += c;
    } else {
      delay(1);
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
        delay(1);
      }
    }
  }
  return true;
}

// отдать HTML
void sendHTML(WiFiClient& client) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/html; charset=UTF-8"));
  client.println(F("Cache-Control: no-store, no-cache, must-revalidate"));
  client.println(F("Pragma: no-cache"));
  client.println(F("Expires: 0"));
  client.println(F("Connection: close"));
  client.println();

  client.println(F("<!doctype html><html><head>"));
  client.println(F("<meta name='viewport' content='width=device-width, initial-scale=1'>"));
  client.println(F("<title>ESP32 Piano Roll</title>"));
  client.println(F("<style>"
    "body{font-family:system-ui,Arial;margin:0;background:#0d0f12;color:#e5e7eb}"
    ".bar{display:flex;align-items:center;gap:12px;padding:10px 12px;background:#111827;position:sticky;top:0;z-index:10}"
    "input,button{font-size:14px;padding:6px 10px;background:#1f2937;color:#e5e7eb;border:1px solid #374151;border-radius:6px}"
    "button{cursor:pointer}"
    "#wrap{padding:10px}"
    "#roll{display:block;background:#111317;border:1px solid #374151;touch-action:none}"
    ".hint{opacity:.8;font-size:12px;padding:8px 12px}"
    "#prog{width:320px}"
    "</style>"));

  client.println(F("<script>'use strict';"));
  client.println(F("let canvas,ctx,W,H;"));
  client.println(F("const rows=24,cols=64;"));
  client.println(F("let cellW=24,cellH=22;"));
  client.println(F("const labelW=72;"));
  client.println(F("let areaW=cols*cellW;"));
  client.println(F("let bpm=120;"));
  client.println(F("const notes=[];"));
  client.println(F("const baseMidi=48;"));
  client.println(F("let drag=null;"));
  client.println(F("let isPlaying=false, playStartT=0, totalMs=0, rafId=0, statusTimer=null;"));

  // длительность по всей сетке
  client.println(F("function calcTotalMs(){"
    "const qMs=Math.floor(60000/Math.max(30,Math.min(240,bpm)));"
    "const cellMs=Math.floor(qMs/4);"
    "totalMs = cols * cellMs;"
    "const p=document.getElementById('prog'); p.max=totalMs>0?totalMs:1; p.value=0;"
    "}"));

  client.println(F("function resize(){"
    "canvas.width = labelW + areaW;"
    "canvas.height= rows*cellH;"
    "W=canvas.width; H=canvas.height;"
    "draw();"
    "}"));

  // сетка и подписи слева
  client.println(F("function drawGrid(){"
    "ctx.fillStyle='#0b0f14'; ctx.fillRect(0,0,labelW,H);"
    "ctx.strokeStyle='#475569'; ctx.beginPath(); ctx.moveTo(labelW+0.5,0); ctx.lineTo(labelW+0.5,H); ctx.stroke();"
    "ctx.fillStyle='#111317'; ctx.fillRect(labelW,0,areaW,H);"
    "ctx.lineWidth=1.5;"
    "for(let c=0;c<=cols;c++){"
      "const x=labelW + c*cellW + 0.5;"
      "ctx.strokeStyle=(c%4===0)?'#475569':'#2d3748';"
      "ctx.beginPath(); ctx.moveTo(x,0); ctx.lineTo(x,H); ctx.stroke();"
    "}"
    "for(let r=0;r<=rows;r++){"
      "const y=H-r*cellH-0.5;"
      "ctx.strokeStyle='#2d3748';"
      "ctx.beginPath(); ctx.moveTo(labelW,y); ctx.lineTo(labelW+areaW,y); ctx.stroke();"
    "}"
    "ctx.textBaseline='middle'; ctx.font='12px system-ui,Arial';"
    "const names=['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];"
    "for(let r=0;r<rows;r++){"
      "const midi=baseMidi+r;"
      "const y=H-(r+0.5)*cellH;"
      "const pc=midi%12;"
      "const isBlack=[1,3,6,8,10].includes(pc);"
      "if(isBlack){ ctx.fillStyle='rgba(255,255,255,0.035)'; ctx.fillRect(labelW,H-(r+1)*cellH,areaW,cellH); }"
      "const oct=Math.floor(midi/12)-1;"
      "ctx.fillStyle=(pc===0)?'#7dd3fc':(isBlack?'#cbd5e1':'#e5e7eb');"
      "ctx.fillText(names[pc]+oct, 8, y);"
    "}"
    "}"));

  // ноты
  client.println(F("function drawNotes(){"
    "for(const n of notes){"
      "const x=labelW + n.col*cellW + 1, y=H-(n.row+1)*cellH+1;"
      "const w=Math.max(4,n.len*cellW-2), h=cellH-2;"
      "ctx.fillStyle='#38bdf8'; ctx.fillRect(x,y,w,h);"
      "ctx.strokeStyle='#0ea5e9'; ctx.lineWidth=1; ctx.strokeRect(x+0.5,y+0.5,w-1,h-1);"
    "}"
    "}"));

  // playhead
  client.println(F("function drawPlayhead(){"
    "if(!isPlaying||totalMs<=0) return;"
    "let t=performance.now()-playStartT;"
    "if(t<0)t=0;"
    "if(t>totalMs)t=totalMs;"
    "document.getElementById('prog').value=Math.floor(t);"
    "const x=labelW + (t/totalMs)*areaW + 0.5;"
    "ctx.strokeStyle='#ef4444'; ctx.lineWidth=2;"
    "ctx.beginPath(); ctx.moveTo(x,0); ctx.lineTo(x,H); ctx.stroke();"
    "}"));

  client.println(F("function draw(){ drawGrid(); drawNotes(); drawPlayhead(); }"));

  // позиция
  client.println(F("function posToCell(px,py){"
    "if(px < labelW) return {row:-1,col:-1};"
    "px -= labelW;"
    "let col=Math.floor(px/cellW), row=Math.floor((H-1-py)/cellH);"
    "if(col<0)col=0; if(col>cols-1)col=cols-1;"
    "if(row<0)row=0; if(row>rows-1)row=rows-1;"
    "return {row,col};"
    "}"));

  client.println(F("function noteAt(col,row){"
    "for(let i=notes.length-1;i>=0;i--){ const n=notes[i];"
      "if(row===n.row && col>=n.col && col<=n.col+n.len-1) return i;"
    "}"
    "return -1;"
    "}"));

  // мышь
  client.println(F("function onDown(e){"
    "const r=canvas.getBoundingClientRect(); const x=e.clientX-r.left, y=e.clientY-r.top;"
    "const p=posToCell(x,y);"
    "if(p.row<0) return;"
    "const idx=noteAt(p.col,p.row);"
    "if(idx>=0){ const n=notes[idx], tail=n.col+n.len-1;"
      "if(p.col>=tail) drag={type:'resize',idx};"
      "else drag={type:'move',idx,dx:p.col-n.col};"
    "}else{"
      "notes.push({row:p.row,col:p.col,len:1});"
      "drag={type:'new',idx:notes.length-1,startX:p.col};"
    "}"
    "draw();"
    "}"));

  client.println(F("function onMove(e){"
    "if(!drag) return;"
    "const r=canvas.getBoundingClientRect();"
    "let x=e.clientX-r.left, y=e.clientY-r.top;"
    "if(x < labelW) x = labelW;"
    "if(x > labelW+areaW-1) x = labelW+areaW-1;"
    "const p=posToCell(x,y);"
    "if(p.row<0) return;"
    "const n=notes[drag.idx];"
    "if(drag.type==='new'){"
      "let len=p.col-drag.startX+1; if(len<1)len=1;"
      "n.row=p.row; n.col=(p.col<drag.startX)?p.col:drag.startX; n.len=len;"
    "} else if (drag.type==='resize'){"
      "let len=p.col-n.col+1; if(len<1)len=1; const maxLen=cols-n.col; if(len>maxLen)len=maxLen; n.len=len;"
    "} else {"
      "let newCol=p.col-drag.dx; if(newCol<0)newCol=0; if(newCol>cols-n.len)newCol=cols-n.len; n.col=newCol; n.row=p.row;"
    "}"
    "draw();"
    "}"));

  client.println(F("function onUp(){ drag=null; }"));

  // BPM
  client.println(F("function setBPM(){"
    "let v=parseInt(document.getElementById('bpm').value||'120');"
    "if(v<30)v=30; if(v>240)v=240;"
    "bpm=v; calcTotalMs(); draw();"
    "}"));

  // опрос /status
  client.println(F("function startStatusPoll(){"
    "if(statusTimer) clearInterval(statusTimer);"
    "statusTimer = setInterval(async ()=>{"
      "try{"
        "const r = await fetch('/status');"
        "const st = await r.json();"
        "const qMs=Math.floor(60000/Math.max(30,Math.min(240,bpm)));"
        "const cellMs=Math.floor(qMs/4);"
        "const gridTotal = cols * cellMs;"
        "if(st.playing){"
          "let el = st.elapsed; if(el > gridTotal) el = gridTotal;"
          "playStartT = performance.now() - el;"
          "totalMs = gridTotal;"
          "document.getElementById('prog').max = gridTotal;"
          "isPlaying = true;"
        "} else {"
          "isPlaying = false;"
        "}"
      "}catch(e){}"
    "}, 200);"
    "}"));

  client.println(F("function stopStatusPoll(){ if(statusTimer){ clearInterval(statusTimer); statusTimer=null; }}"));

  // play
  client.println(F("async function play(){"
    "setBPM();"
    "const qMs=Math.floor(60000/bpm);"
    "const cellMs=Math.floor(qMs/4);"
    "let body='';"
    "for(const n of notes){"
      "const midi=baseMidi+n.row;"
      "const startMs=n.col*cellMs;"
      "const durMs=n.len*cellMs;"
      "body+=midi+','+startMs+','+durMs+';';"
    "}"
    "try{"
      "await fetch('/play',{method:'POST',headers:{'Content-Type':'text/plain'},body});"
      "isPlaying=true;"
      "playStartT = performance.now();"
      "startStatusPoll();"
      "if(rafId) cancelAnimationFrame(rafId);"
      "(function loop(){ draw(); if(isPlaying) rafId=requestAnimationFrame(loop); })();"
    "}catch(e){ console.error('play failed', e); }"
    "}"));

  // stop
  client.println(F("async function stopPlay(){"
    "isPlaying=false;"
    "stopStatusPoll();"
    "if(rafId) cancelAnimationFrame(rafId);"
    "document.getElementById('prog').value=0;"
    "try{ await fetch('/stop'); }catch(e){}"
    "draw();"
    "}"));

  // init
  client.println(F("window.addEventListener('load',()=>{"
    "canvas=document.getElementById('roll');"
    "ctx=canvas.getContext('2d');"
    "document.getElementById('bpm').value=bpm;"
    "calcTotalMs();"
    "resize();"
    "window.addEventListener('resize', resize);"
    "canvas.addEventListener('mousedown', onDown);"
    "window.addEventListener('mousemove', onMove);"
    "window.addEventListener('mouseup', onUp);"
    "draw();"
    "});"));

  client.println(F("</script></head><body>"));
  client.println(F("<div class='bar'>"
    "<label>BPM <input id='bpm' type='number' min='30' max='240' value='120' oninput='setBPM()'></label>"
    "<button onclick='play()'>Play</button>"
    "<button onclick='stopPlay()'>Stop</button>"
    "<input id='prog' type='range' min='0' max='1' value='0' step='1' disabled>"
    "<span class='hint'>ESP32 в режиме STA. Открой IP, который она напишет в Serial.</span>"
    "</div>"));
  client.println(F("<div id='wrap'><canvas id='roll' width='1700' height='528'></canvas></div>"));
  client.println(F("</body></html>"));
  client.println();
}

void setup() {
  Serial.begin(115200);
  pinMode(BEEP_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  noTone(BEEP_PIN);
  digitalWrite(LED_PIN, LOW);

  // режим STA
  WiFi.mode(WIFI_STA);
  Serial.print("Connecting to "); Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // ждём до 10 сек
  uint8_t tries = 40;
  while (WiFi.status() != WL_CONNECTED && tries--) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Check SSID/PASS.");
    // сервер не поднимаем, потому что смысла нет
  } else {
    Serial.println("WiFi connected.");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    server.begin();
  }
}

void loop() {
  playerTick();

  if (WiFi.status() != WL_CONNECTED) {
    // тут можно добавить переподключение, если у тебя дома Wi-Fi как настроение
    return;
  }

  WiFiClient client = server.available();
  if (!client) return;

  String method, path, body;
  int contentLen = 0;
  if (!readRequest(client, method, path, contentLen, body)) {
    client.println(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"));
    client.stop();
    return;
  }

  if (method == "GET" && (path == "/" || path.startsWith("/index"))) {
    sendHTML(client);
  }
  else if (method == "POST" && path.startsWith("/play")) {
    bool ok = parseBodyIntoEvents(body);
    if (ok) {
      startPlay();
      client.println(F("HTTP/1.1 200 OK"));
      client.println(F("Content-Type: text/plain"));
      client.println(F("Connection: close"));
      client.println();
      client.println(F("Playing"));
    } else {
      stopPlay();
      client.println(F("HTTP/1.1 400 Bad Request"));
      client.println(F("Content-Type: text/plain"));
      client.println(F("Connection: close"));
      client.println();
      client.println(F("No events"));
    }
  }
  else if (method == "GET" && path.startsWith("/stop")) {
    stopPlay();
    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: text/plain"));
    client.println(F("Connection: close"));
    client.println();
    client.println(F("Stopped"));
  }
  else if (method == "GET" && path.startsWith("/status")) {
    uint32_t elapsed = 0;
    if (playing) {
      elapsed = millis() - playStartMs;
    }
    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: application/json"));
    client.println(F("Cache-Control: no-store"));
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
}
