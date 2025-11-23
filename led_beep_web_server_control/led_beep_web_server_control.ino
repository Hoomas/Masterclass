// Загружаем библиотеку Wi-Fi
#include <WiFi.h>

// Замените на свой идентификатор и пароль
const char* ssid = "BKZ_MKZ";
const char* password = "cudo_BKZ_MKZ";

#define led 35
#define beep 7

// Номер порта для сервера
WiFiServer server(80);

// HTTP-запрос
String header;

// текущее состояние кнопки
String beepState = "off";
String ledState = "off";

// Номера выводов


void setup() {
  Serial.begin(115200);
  // Настраиваем выводы платы
  pinMode(led, OUTPUT);
  pinMode(beep, OUTPUT);
  // Переводим выводы в LOW
  digitalWrite(led, LOW);
  noTone(beep);
  // Подключаемся к Wi-Fi
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  // Выводим локальный IP-адрес и запускаем сервер
  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  server.begin();
}

void loop() {
  WiFiClient client = server.available(); // прослушка входящих клиентов
  if (client) { // Если подключается новый клиент,
    Serial.println("New Client."); // выводим сообщение
    String currentLine = "";
    while (client.connected()) { // цикл, пока есть соединение клиента
      if (client.available()) { // если от клиента поступают данные,
        char c = client.read(); // читаем байт, затем
        Serial.write(c); // выводим на экран
        header += c;
        if (c == '\n') { // если байт является переводом строки
          // если пустая строка, мы получили два символа перевода строки
          // значит это конец HTTP-запроса, формируем ответ сервера:
          if (currentLine.length() == 0) {
            // HTTP заголовки начинаются с кода ответа (напр., HTTP / 1.1 200 OK)
            // и content-type, затем пустая строка:
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println("Connection: close");
            client.println();

            // Включаем или выключаем светодиоды
            if (header.indexOf("GET /beep/on") >= 0) {
              Serial.println("beep on");
              beepState = "on";
              tone(beep, 1000);  // пищать на пине 3, 1 кГц
              delay(500);
            } else if (header.indexOf("GET /beep/off") >= 0) {
              Serial.println("beep off");
              beepState = "off";
              noTone(beep);
              delay(500);
            } else if (header.indexOf("GET /led/on") >= 0) {
              Serial.println("led on");
              ledState = "on";
              digitalWrite(led, HIGH);
            } else if (header.indexOf("GET /led/off") >= 0) {
              Serial.println("led off");
              ledState = "off";
              digitalWrite(led, LOW);
            }
            // Формируем веб-страницу на сервере
            client.println("<!DOCTYPE html><html>");
            client.println("<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
            client.println("<link rel=\"icon\" href=\"data:,\">");
            // CSS для кнопок
            // можете менять под свои нужды
            client.println("<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}");
            client.println(".button { background-color: #4CAF50; border:  none; color: white; padding: 16px 40px;");
            client.println("text-decoration: none; font-size: 30px; margin: 2px; cursor: pointer;}");
            client.println(".button2 {background-color:#555555;}</style></head>");
            client.println("<body><h1>ESP32 Web Server</h1>");
            // Выводим текущее состояние кнопок
            client.println("<p>beep_State " + beepState + "</p>");
            // Если beepState сейчас off, то выводим надпись ON
            if (beepState == "off") {
              client.println("<p><a href=\"/beep/on\"><button class=\"button button2\">OFF</button></a></p>");
            } else {
              client.println("<p><a href=\"/beep/off\"><button class=\"button\">ON</button></a></p>");
            }
            // Аналогично для второй кнопки
            client.println("<p>led - State " + ledState + "</p>");
            if (ledState == "off") {
              client.println("<p><a href=\"/led/on\"><button class=\"button button2\">OFF</button></a></p>");
            } else {
              client.println("<p><a href=\"/led/off\"><button class=\"button\">ON</button></a></p>");
            }
            client.println("</body></html>");
            // HTTP-ответ завершается пустой строкой
            client.println();
            break;
          } else { // если получили новую строку, очищаем currentLine
            currentLine = "";
          }
        } else if (c != '\r') { // Если получили что-то ещё кроме возврата строки,
          currentLine += c; // добавляем в конец currentLine
        }
      }
    }
    // Очистим переменную
    header = "";
    // Закрываем соединение
    client.stop();
    Serial.println("Client disconnected.");
    Serial.println("");
  }
}