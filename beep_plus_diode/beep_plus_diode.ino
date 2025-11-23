#define LED_PIN 35     // светодиод
#define BEEP_PIN 7   // пассивный бипер

void setup() {
  pinMode(LED_PIN, OUTPUT);
  pinMode(BEEP_PIN, OUTPUT);
}

void loop() {
  // включаем лампу и звук
  digitalWrite(LED_PIN, HIGH);
  //tone(BEEP_PIN, 1000); // частота 1 кГц
  delay(200);           // держим 0,2 сек

  // выключаем
  digitalWrite(LED_PIN, LOW);
  //noTone(BEEP_PIN);
  delay(300);           // пауза 0,3 сек

  // повторяем (мигает и пищит)
}
