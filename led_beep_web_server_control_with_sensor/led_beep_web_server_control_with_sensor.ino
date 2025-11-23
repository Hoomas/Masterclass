

#include <WiFi.h>

// ---------- WiFi ----------
const char* ssid     = "iPhone";   // замени при необходимости
const char* password = "11111111";   // замени при необходимости

// ---------- Пины (безопасные для ESP32-S3 DevKitC) ----------
#define LED_PIN   10
#define BEEP_PIN  15
#define TRIG_PIN  4
#define ECHO_PIN  5   //5V->3.3V!

// ---------- Сервер ----------
WiFiServer server(80);
String header;

// ---------- Состояния ----------
String ledState  = "off";
String beepState = "off";

// Кэш последнего измерения для быстрой отрисовки страницы
static float lastDistanceCm = NAN;
static uint32_t lastMeasureMs = 0;

// ---------- Измерение расстояния (HC-SR04), результат в сантиметрах ----------
float readDistanceCm(uint32_t timeout_us = 25000) {
  // импульс триггера 10 мкс
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // длительность высокого уровня на ECHO (таймаут ~25 мс ≈ 4+ метра)
  uint32_t dur = pulseIn(ECHO_PIN, HIGH, timeout_us);
  if (dur == 0) return NAN;         // таймаут/нет эха
  return dur / 58.0f;               // формула HC-SR04: см ≈ мкс / 58
}

// ---------- Ответ: главная страница ----------
void sendIndexPage(WiFiClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:text/html; charset=UTF-8");
  client.println("Connection: close");
  client.println();

  client.println("<!DOCTYPE html><html><head>"
                 "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                 "<style>"
                 "html{font-family:Helvetica;text-align:center}"
                 ".button{background:#4CAF50;color:#fff;padding:16px 40px;font-size:30px;border:none;margin:2px;cursor:pointer}"
                 ".button2{background:#555}"
                 ".card{display:inline-block;margin:10px;padding:16px;border:1px solid #ccc;border-radius:12px;min-width:260px}"
                 ".val{font-size:42px;margin:10px 0}"
                 "</style>"
                 "<script>"
                 "async function poll(){"
                   "try{let r=await fetch('/sensor');"
                       "let j=await r.json();"
                       "document.getElementById('dist').textContent="
                         "(j.distance_cm===null?'—':j.distance_cm.toFixed(1))+' cm';"
                   "}catch(e){}"
                 "}"
                 "setInterval(poll,500);"
                 "window.onload=poll;"
                 "</script>"
                 "</head><body>");

  client.println("<h1>ESP32-S3 Web Server</h1>");

  // Sensor Data карточка
  client.println("<div class='card'><h2>Sensor Data</h2>"
                 "<div id='dist' class='val'>"
                 + String(isnan(lastDistanceCm) ? "—" : String(lastDistanceCm, 1) + " cm")
                 + "</div></div>");

  // Buzzer карточка
  client.println("<div class='card'><h2>Buzzer</h2>"
                 "<p>Status: " + beepState + "</p>");
  if (beepState == "off") {
    client.println("<p><a href='/beep/on'><button class='button button2'>BEEP OFF</button></a></p>");
  } else {
    client.println("<p><a href='/beep/off'><button class='button'>BEEP ON</button></a></p>");
  }
  client.println("</div>");

  // LED карточка
  client.println("<div class='card'><h2>LED</h2>"
                 "<p>Status: " + ledState + "</p>");
  if (ledState == "off") {
    client.println("<p><a href='/led/on'><button class='button button2'>LED OFF</button></a></p>");
  } else {
    client.println("<p><a href='/led/off'><button class='button'>LED ON</button></a></p>");
  }
  client.println("</div>");

  client.println("</body></html>");
  client.println();
}


void sendSensorJson(WiFiClient &client, float d) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json; charset=UTF-8");
  client.println("Cache-Control: no-cache");
  client.println("Connection: close");
  client.println();
  if (isnan(d)) {
    client.println("{\"distance_cm\":null}");
  } else {
    client.print("{\"distance_cm\":");
    client.print(d, 2);
    client.println("}");
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  pinMode(BEEP_PIN, OUTPUT);
  noTone(BEEP_PIN);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  Serial.print("Connecting to "); Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  server.begin();
}

void loop() {
  // Периодический замер каждые ~200 мс для кэша страницы
  if (millis() - lastMeasureMs >= 200) {
    lastMeasureMs = millis();
    lastDistanceCm = readDistanceCm();
    // опционально: авто-писк, если близко (например, <= 20 см) и звук включен
    if (!isnan(lastDistanceCm) && lastDistanceCm > 0 && lastDistanceCm <= 20.0 && beepState == "on") {
      tone(BEEP_PIN, 2000, 120);   // короткий писк 120 мс на 2 кГц
    }
  }

  WiFiClient client = server.available();
  if (!client) return;

  Serial.println("New Client");
  String currentLine;
  header = "";
  uint32_t t0 = millis();

  while (client.connected()) {
    if (client.available()) {
      char c = client.read();
      header += c;

      if (c == '\n') {
        if (currentLine.length() == 0) {
          // ---- маршруты ----
          if (header.indexOf("GET /beep/on") >= 0) {
            beepState = "on";
            tone(BEEP_PIN, 2000);
          } else if (header.indexOf("GET /beep/off") >= 0) {
            beepState = "off";
            noTone(BEEP_PIN);
          } else if (header.indexOf("GET /led/on") >= 0) {
            ledState = "on";
            digitalWrite(LED_PIN, HIGH);
          } else if (header.indexOf("GET /led/off") >= 0) {
            ledState = "off";
            digitalWrite(LED_PIN, LOW);
          } else if (header.indexOf("GET /sensor") >= 0) {
            float d = readDistanceCm();
            sendSensorJson(client, d);
            break;
          }

          // страница по умолчанию
          sendIndexPage(client);
          break;
        } else {
          currentLine = "";
        }
      } else if (c != '\r') {
        currentLine += c;
      }

      // защита от разрастания заголовка
      if (header.length() > 4096) header.remove(0, 2048);
    }

    if (millis() - t0 > 6000) break;  // таймаут сессии
    delay(1);                         // кормим WDT
  }

  client.stop();
  Serial.println("Client disconnected");
}
