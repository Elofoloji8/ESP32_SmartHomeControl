#include <WiFi.h>
#include <ESP32Servo.h>
#include <HardwareSerial.h>
#include <stdint.h>
#include <Firebase_ESP_Client.h>

// ===== FIREBASE =====
#define FIREBASE_HOST "https://smarthomecontrol-31eb5-default-rtdb.firebaseio.com/"
#define FIREBASE_AUTH "AIzaSyDOLBbciMkQWoCwSh4xtH9o92FnH-DKddE"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long lastFirebaseSend = 0;

// ===== UART (ARDUINO UNO) =====
HardwareSerial ArduinoSerial(2); // UART2 (RX=16, TX=17)

// ===== ARDUINO -> ESP VERİ PAKETİ ===== 
struct __attribute__((packed)) VeriPaketi {
  uint8_t header;
  int16_t sicak_x10;
  uint8_t fanSeviyesi;
  int yagmurDurumu;
  int camasirlikDurumu; 
};
// ===== WIFI =====
const char* ssid = "A51";
const char* password = "12345678";

// ===== TCP =====
WiFiServer server(8080);

//==== SERVO PIN ====
#define SERVO_PIN 2
Servo doorServo;

// ==== GAS PIN ====
#define GAS_PIN 32
int lastGasState = -1; // -1 = ilk okuma, 0 = yok, 1 = var

// ==== WATER LEVEL ====
#define WATER_PIN 34
#define WATER_MIN 800
#define WATER_MAX 3500
int lastWaterPercent = -1; // ilk okuma için

// ==== PIR SENSOR ====
#define PIR_PIN 13
int lastPirState = -1; // -1: ilk okuma, 0: yok, 1: var

// 🔥 UART DATA
float uartSicaklik = NAN;
uint8_t uartFanSeviyesi = 0;
int uartYagmurDurumu = 0;
int uartCamasirlikDurumu = 0;
// ⏱️ Son veri zamanı (kopma tespiti için)
unsigned long lastUartMillis = 0;

// ===== RGB PIN =====
#define RED_PIN   26
#define GREEN_PIN 27
#define BLUE_PIN  25

#define RED_CH   0
#define GREEN_CH 1
#define BLUE_CH  2

// ===================== FIREBASE SEND ===================== 
void firebaseSicaklikGonder(float sicaklik) {
  if (isnan(sicaklik)) return;

  String path = "/sensors/temperature_log";

  if (Firebase.ready()){
    if (Firebase.RTDB.pushFloat(&fbdo, path, sicaklik)) {
      Serial.println("📤 Firebase → Sicaklik gonderildi");
    } else {
      Serial.print("❌ Firebase HATA: ");
      Serial.println(fbdo.errorReason());
    }
  }
}


void setup() {
  Serial.begin(9600);
  delay(1000);

  // ===== UART SETUP =====
  ArduinoSerial.begin(9600, SERIAL_8N1, 16, 17);
  Serial.println("🔌 UART (Arduino) READY");

  Serial.println("Connecting WiFi.");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi CONNECTED");
  Serial.println(WiFi.localIP());

  // PWM
  ledcSetup(RED_CH, 5000, 8);
  ledcSetup(GREEN_CH, 5000, 8);
  ledcSetup(BLUE_CH, 5000, 8);

  ledcAttachPin(RED_PIN, RED_CH);
  ledcAttachPin(GREEN_PIN, GREEN_CH);
  ledcAttachPin(BLUE_PIN, BLUE_CH);

  // LED başlangıçta kapalı
  ledcWrite(RED_CH, 0);
  ledcWrite(GREEN_CH, 0);
  ledcWrite(BLUE_CH, 0);

  // ===== SERVO =====
  doorServo.attach(SERVO_PIN);
  doorServo.write(0);   // başlangıç kapalı

  // ==== GAS ====
  pinMode(GAS_PIN, INPUT);

  // === WATER ===
  pinMode(WATER_PIN, INPUT);

  // === PIR ===
  pinMode(PIR_PIN, INPUT);

  // ===== SERVER =====
  server.begin();
  Serial.println("🔥 TCP RGB + SERVO SERVER READY");

  // ===== FIREBASE INIT =====
  config.api_key = FIREBASE_AUTH;
  config.database_url = FIREBASE_HOST;

// 🔐 EMAIL / PASSWORD AUTH
config.api_key = FIREBASE_AUTH;
config.database_url = FIREBASE_HOST;

Firebase.begin(&config, &auth);
Firebase.reconnectWiFi(true);

// 🔑 Anonymous sign-in
if (Firebase.signUp(&config, &auth, "", "")) {
  Serial.println("🔥 Firebase Anonymous Auth OK");
} else {
  Serial.printf("❌ Auth HATA: %s\n", config.signer.signupError.message.c_str());
}

// Timeout ayarları (önerilir)
  config.timeout.serverResponse = 10 * 1000;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("🔥 Firebase connected (AUTH OK)");
}

void loop() {
  WiFiClient client = server.available();

  if (client) {
    Serial.println("📥 Client connected");

    while (client.connected()) {
      if (client.available()) {
        String msg = client.readStringUntil('\n');
        msg.trim();
        Serial.println("📨 RAW DATA: " + msg);

        // ===== LED OFF =====
        if (msg == "OFF") {
          ledcWrite(RED_CH, 0);
          ledcWrite(GREEN_CH, 0);
          ledcWrite(BLUE_CH, 0);
          Serial.println("💡 LED OFF");
          continue;
        }

        // ===== SERVO OPEN =====
        if (msg == "SERVO:OPEN") {
          doorServo.write(90);
          Serial.println("🚪 SERVO OPEN (90°)");
          continue;
        }

        // ===== SERVO CLOSE =====
        if (msg == "SERVO:CLOSE") {
          doorServo.write(0);
          Serial.println("🚪 SERVO CLOSE (0°)");
          continue;
        }

        // ===== GAS SENSOR REQUEST =====
        if (msg == "GAS?") {
          int gasValue = digitalRead(GAS_PIN);

          if (gasValue == LOW) {
            client.println("GAS:VAR");
          } else {
            client.println("GAS:YOK");
          }
          continue;
        }

        // ===== WATER LEVEL REQUEST =====
        if (msg == "WATER?") {
          int rawValue = analogRead(WATER_PIN);
          rawValue = constrain(rawValue, WATER_MIN, WATER_MAX);
          int waterPercent = map(rawValue, WATER_MIN, WATER_MAX, 0, 100);

        client.print("WATER:");
        client.println(waterPercent);

        Serial.print("💧 TCP WATER: ");
        Serial.print(waterPercent);
        Serial.println(" %");

        continue;
        }

        // ===== PIR SENSOR REQUEST =====
        if (msg == "PIR?") {
          int pirValue = digitalRead(PIR_PIN);

        if (pirValue == HIGH) {
          client.println("PIR:VAR");
        } else {
         client.println("PIR:YOK");
        }
        continue;
        }

        // ===== RGB =====
        int r, g, b;
        if (sscanf(msg.c_str(), "%d,%d,%d", &r, &g, &b) == 3) {
          r = constrain(r, 0, 255);
          g = constrain(g, 0, 255);
          b = constrain(b, 0, 255);

          ledcWrite(RED_CH, r);
          ledcWrite(GREEN_CH, g);
          ledcWrite(BLUE_CH, b);

          Serial.printf("🎨 RGB SET → %d %d %d\n", r, g, b);
        }
      }
    }

    client.stop();
    Serial.println("❌ Client disconnected");
  }

  // =====================================================
  // 🔥 GAS SENSOR SERIAL LOG (TEK SEFERLİK – DURUM DEĞİŞİNCE)
  // =====================================================
  int gasValue = digitalRead(GAS_PIN);
  int currentGasState = (gasValue == LOW) ? 1 : 0; // LOW = gaz var

  if (currentGasState != lastGasState) {
    if (currentGasState == 1) {
      Serial.println("⛽ GAZ VAR");
    } else {
      Serial.println("⛽ GAZ YOK");
    }
    lastGasState = currentGasState;
  }

// =====================================================
// 💧 WATER LEVEL SERIAL LOG (SADECE DEĞİŞİNCE)
// =====================================================
  static unsigned long lastWaterRead = 0;

  if (millis() - lastWaterRead > 10000) {   // 10 saniyede bir ölç
    lastWaterRead = millis();

    int rawValue = analogRead(WATER_PIN);
    rawValue = constrain(rawValue, WATER_MIN, WATER_MAX);

    int waterPercent = map(rawValue, WATER_MIN, WATER_MAX, 0, 100);

    if (waterPercent != lastWaterPercent) {
      Serial.print("💧 Su Seviyesi: ");
      Serial.print(waterPercent);
      Serial.println(" %");

      lastWaterPercent = waterPercent;
    }
  }


  // =====================================================
  // 🔥 FIREBASE: SICAKLIK GÖNDER (5 SN)
  // =====================================================
  if (!isnan(uartSicaklik) && millis() - lastFirebaseSend > 5000) {

    firebaseSicaklikGonder(uartSicaklik);
    lastFirebaseSend = millis();
  }


  // =====================================================
  // 🚶 PIR SENSOR SERIAL LOG (SADECE DEĞİŞİNCE)
  // =====================================================
  int pirValue = digitalRead(PIR_PIN);
  int currentPirState = (pirValue == HIGH) ? 1 : 0;

  if (currentPirState != lastPirState) {
    if (currentPirState == 1) {
      Serial.println("🚶 HAREKET ALGILANDI");
    } else {
      Serial.println("🚶 HAREKET YOK");
    }
    lastPirState = currentPirState;
  }

  // =====================================================
// 🔄 UART: ARDUINO -> ESP32 SICAKLIK OKUMA
// =====================================================
    if (ArduinoSerial.available() >= sizeof(VeriPaketi)) {

  // Header kontrolü
      if (ArduinoSerial.peek() != 0xAA) {
        ArduinoSerial.read(); // bozuk byte temizle
      }

      VeriPaketi gelen;
      if (ArduinoSerial.readBytes((uint8_t*)&gelen, sizeof(gelen)) == sizeof(gelen)) {
        uartSicaklik = gelen.sicak_x10 / 10.0f;
        uartFanSeviyesi = gelen.fanSeviyesi;
        uartYagmurDurumu = gelen.yagmurDurumu;
        uartCamasirlikDurumu = gelen.camasirlikDurumu;
        lastUartMillis = millis();

        Serial.print("📥 UART SICAKLIK: ");
        Serial.print(uartSicaklik);
        Serial.print(" °C | FAN: ");
        Serial.println(uartFanSeviyesi);
        Serial.println("------------------------------------");
        if (uartYagmurDurumu != 0)
        {Serial.print("Yağmur durumu : Yagmur var");
        Serial.println ("Çamaşırlık açıldı.")}
        else{Serial.print("Yağmur durumu : Yagmur yok");
         Serial.println ("Çamaşırlık açıldı.")}
      }
    }

    if (millis() - lastUartMillis > 5000) {
      uartSicaklik = NAN;
    }
}
