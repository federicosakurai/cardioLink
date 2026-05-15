// ============================================================
//  PULSONET — ECG GATEWAY
//  ESP32 WROVER
//  Recibe trama HL7 por RS232, extrae el PDF en Base64
//  y lo envía a Firebase Realtime Database via WiFi
//
//  Conexión física:
//  ECG RS232 TX  →  MAX3232  →  GPIO 16 (RX2) del ESP32
//  ECG RS232 RX  →  MAX3232  →  GPIO 17 (TX2) del ESP32
//  MAX3232 VCC   →  3.3V
//  MAX3232 GND   →  GND
//
//  IMPORTANTE: En Arduino IDE habilitar
//  Tools → PSRAM → Enabled
// ============================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino.h>

// ── Configuración WiFi ───────────────────────────────────────
const char* WIFI_SSID     = "TU_RED_WIFI";
const char* WIFI_PASSWORD = "TU_PASSWORD";

// ── Configuración Firebase ───────────────────────────────────
const char* FIREBASE_URL  = "https://firestore.googleapis.com/v1/projects/cardiolink-f186f/databases/(default)/documents/ecgs";
const char* FIREBASE_AUTH = "";

// ── Configuración RS232 / UART2 ──────────────────────────────
#define RXD2        16
#define TXD2        17
#define BAUD_RS232  9600

// ── Buffer en PSRAM ──────────────────────────────────────────
#define BUFFER_SIZE (200 * 1024)  // 200 KB en PSRAM
char* hl7Buffer       = nullptr;
int   hl7Len          = 0;
bool  recibiendoTrama = false;

// ── MLLP: delimitadores estándar HL7 ────────────────────────
#define MLLP_START  0x0B
#define MLLP_END    0x1C


// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== PULSONET — ECG GATEWAY WiFi ===");

  // ── Verificar e inicializar PSRAM ──────────────────────────
  // No hay que declarar nada, el WROVER la detecta solo.
  // Solo hay que habilitar Tools → PSRAM → Enabled en Arduino IDE
  if (!psramFound()) {
    Serial.println("ERROR: PSRAM no detectada.");
    Serial.println("Verificar: Tools → PSRAM → Enabled en Arduino IDE");
    while (true) delay(1000);
  }

  // Alocar buffer en PSRAM (no usa RAM interna)
  hl7Buffer = (char*) ps_malloc(BUFFER_SIZE);
  if (hl7Buffer == nullptr) {
    Serial.println("ERROR: No se pudo alocar memoria en PSRAM.");
    while (true) delay(1000);
  }
  Serial.printf("PSRAM OK — Buffer: %d KB alocados en PSRAM\n", BUFFER_SIZE / 1024);
  Serial.printf("PSRAM libre: %d bytes\n", ESP.getFreePsram());

  // ── Inicializar UART2 para RS232 ───────────────────────────
  Serial2.begin(BAUD_RS232, SERIAL_8N1, RXD2, TXD2);
  Serial.printf("RS232 OK — Baudrate: %d | RX: GPIO%d | TX: GPIO%d\n",
                BAUD_RS232, RXD2, TXD2);

  // ── Conectar WiFi ──────────────────────────────────────────
  conectarWifi();

  Serial.println("=== ESPERANDO TRAMA DEL ECG ===\n");
}


// ============================================================
//  LOOP PRINCIPAL
// ============================================================
void loop() {
  while (Serial2.available()) {
    char c = Serial2.read();

    // Inicio de trama MLLP (0x0B)
    if (c == MLLP_START) {
      hl7Len = 0;
      memset(hl7Buffer, 0, BUFFER_SIZE);
      recibiendoTrama = true;
      Serial.println(">> Inicio de trama detectado (MLLP 0x0B)");
    }

    // Fin de trama MLLP (0x1C)
    else if (c == MLLP_END && recibiendoTrama) {
      hl7Buffer[hl7Len] = '\0';
      recibiendoTrama = false;
      Serial.printf(">> Trama completa: %d bytes\n", hl7Len);
      procesarTrama();
    }

    // Acumular bytes en PSRAM
    else if (recibiendoTrama) {
      if (hl7Len < BUFFER_SIZE - 1) {
        hl7Buffer[hl7Len++] = c;
      } else {
        Serial.println("ERROR: Buffer lleno, trama demasiado grande.");
        recibiendoTrama = false;
      }
    }
  }

  // ── Si el ECG no usa MLLP descomenta esto ─────────────────
  // recibirSinMLLP();
}


// ============================================================
//  PROCESAR TRAMA HL7
// ============================================================
void procesarTrama() {
  // Extraer datos del paciente para identificar el registro
  String paciente = extraerCampo("PID", 5);
  String fecha    = extraerCampo("OBR", 7);
  String ecgId    = extraerCampo("OBR", 3);
  paciente.replace("^", "_");

  Serial.printf("Paciente: %s | ID: %s | Fecha: %s\n",
                paciente.c_str(), ecgId.c_str(), fecha.c_str());

  // Extraer el Base64 del PDF
  char* pdfBase64 = extraerBase64PDF(hl7Buffer);
  if (pdfBase64 == nullptr) {
    Serial.println("ERROR: PDF no encontrado en trama.");
    return;
  }

  Serial.printf("Base64 extraído: %d caracteres\n", strlen(pdfBase64));

  // Enviar a Firebase
  enviarAFirebase(pdfBase64, paciente, ecgId, fecha);
}


// ============================================================
//  EXTRAER BASE64 DEL PDF
//  Busca la firma universal "JVBERi0" (= %PDF- en Base64)
//  Corta al primer delimitador HL7
// ============================================================
char* extraerBase64PDF(char* hl7) {
  char* inicio = strstr(hl7, "JVBERi0");

  if (inicio == nullptr) {
    Serial.println("Firma JVBERi0 no encontrada en la trama.");
    return nullptr;
  }

  char* fin = inicio;
  while (*fin != '\0'  &&
         *fin != '|'   &&
         *fin != '\r'  &&
         *fin != '\n'  &&
         *fin != 0x1C) {
    fin++;
  }
  *fin = '\0';

  return inicio;
}


// ============================================================
//  EXTRAER UN CAMPO DE UN SEGMENTO HL7
// ============================================================
String extraerCampo(const char* segmento, int campoNum) {
  char* pos = hl7Buffer;

  while ((pos = strstr(pos, segmento)) != nullptr) {
    if (pos == hl7Buffer || *(pos - 1) == '\n' || *(pos - 1) == '\r') {
      int pipes = 0;
      while (*pos && pipes < campoNum) {
        if (*pos == '|') pipes++;
        pos++;
      }
      char temp[256] = {0};
      int i = 0;
      while (*pos && *pos != '|' && *pos != '\r' && *pos != '\n' && i < 255) {
        temp[i++] = *pos++;
      }
      return String(temp);
    }
    pos++;
  }
  return String("DESCONOCIDO");
}


// ============================================================
//  ENVIAR A FIREBASE VIA WiFi
//  Manda el JSON directo al HTTPClient sin crear String grande
//  El pdfBase64 (132KB) viaja directo desde PSRAM
// ============================================================
void enviarAFirebase(char* pdfBase64, String paciente,
                     String ecgId, String fecha) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desconectado, reconectando...");
    conectarWifi();
  }

  String url = String(FIREBASE_URL) + "/ecgs/" + ecgId + ".json";
  url += "?auth=" + String(FIREBASE_AUTH);

  Serial.println("Enviando a Firebase...");

  // Calcular tamaño total del JSON sin copiar nada a RAM interna
  int jsonLen = 14                    // {"paciente":"
              + paciente.length()
              + 13                    // ","ecg_id":"
              + ecgId.length()
              + 11                    // ","fecha":"
              + fecha.length()
              + 15                    // ","timestamp":
              + 10                    // valor millis
              + 16                    // ,"pdf_base64":"
              + strlen(pdfBase64)     // el Base64 directo desde PSRAM
              + 2;                    // "}

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  // Stream del JSON directo — el pdfBase64 va desde PSRAM
  // sin crear ninguna copia en RAM interna
  WiFiClient* stream = http.getStreamPtr();

  stream->print("{\"paciente\":\"");
  stream->print(paciente);
  stream->print("\",\"ecg_id\":\"");
  stream->print(ecgId);
  stream->print("\",\"fecha\":\"");
  stream->print(fecha);
  stream->print("\",\"timestamp\":");
  stream->print(millis());
  stream->print(",\"pdf_base64\":\"");
  stream->print(pdfBase64);   // char* directo desde PSRAM
  stream->print("\"}");

  int httpCode = http.PUT((uint8_t*)nullptr, jsonLen);

  if (httpCode == 200 || httpCode == 201) {
    Serial.println("✓ ECG enviado correctamente a Firebase");
  } else {
    Serial.printf("✗ Error HTTP: %d\n", httpCode);
    Serial.println(http.getString());
  }

  http.end();
}


// ============================================================
//  RECIBIR SIN MLLP (HL7 plano por RS232)
//  Usar si el ECG no usa MLLP — llamar desde loop()
// ============================================================
void recibirSinMLLP() {
  while (Serial2.available()) {
    char c = Serial2.read();

    if (!recibiendoTrama) {
      if (c == 'M') {
        hl7Buffer[0] = c;
        hl7Len = 1;
        recibiendoTrama = true;
      }
    } else {
      if (hl7Len < BUFFER_SIZE - 1) {
        hl7Buffer[hl7Len++] = c;
      }
      if (hl7Len > 4 &&
          hl7Buffer[hl7Len-1] == '\n' &&
          hl7Buffer[hl7Len-2] == '\n') {
        hl7Buffer[hl7Len] = '\0';
        recibiendoTrama = false;
        Serial.printf(">> Trama plana recibida: %d bytes\n", hl7Len);
        procesarTrama();
      }
    }
  }
}


// ============================================================
//  CONECTAR WIFI CON REINTENTOS
// ============================================================
void conectarWifi() {
  Serial.printf("Conectando a WiFi: %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 20) {
    delay(500);
    Serial.print(".");
    intentos++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("RAM interna libre: %d bytes\n", ESP.getFreeHeap());
  } else {
    Serial.println("\nERROR: No se pudo conectar al WiFi.");
  }
}
