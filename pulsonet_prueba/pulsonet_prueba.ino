// ============================================================
//  PULSONET — MODO PRUEBA SIN ELECTROCARDIÓGRAFO
//  ESP32 WROVER
//
//  La trama HL7 está guardada en el archivo hl7_trama.h
//  que debe estar en la misma carpeta que este .ino
//
//  IMPORTANTE: En Arduino IDE habilitar
//  Tools → PSRAM → Enabled
// ============================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino.h>
#include <pgmspace.h>
#include "hl7_trama.h"   // <-- la trama HL7 completa en Flash

// ── Configuración WiFi ───────────────────────────────────────
const char* WIFI_SSID     = "TU_RED_WIFI";
const char* WIFI_PASSWORD = "TU_PASSWORD";

// ── Configuración Firebase Firestore ────────────────────────
const char* FIREBASE_URL = "https://firestore.googleapis.com/v1/projects/cardiolink-f186f/databases/(default)/documents/ecgs";

// ── Buffer en PSRAM ──────────────────────────────────────────
#define BUFFER_SIZE (200 * 1024)
char* hl7Buffer = nullptr;


// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== PULSONET — MODO PRUEBA ===");

  // ── Inicializar PSRAM ──────────────────────────────────────
  if (!psramFound()) {
    Serial.println("ERROR: PSRAM no detectada.");
    Serial.println("Verificar: Tools → PSRAM → Enabled en Arduino IDE");
    while (true) delay(1000);
  }

  hl7Buffer = (char*) ps_malloc(BUFFER_SIZE);
  if (hl7Buffer == nullptr) {
    Serial.println("ERROR: No se pudo alocar memoria en PSRAM.");
    while (true) delay(1000);
  }
  Serial.printf("PSRAM OK — %d KB alocados\n", BUFFER_SIZE / 1024);
  Serial.printf("PSRAM libre: %d bytes\n", ESP.getFreePsram());

  // ── Copiar trama HL7 desde Flash (PROGMEM) a PSRAM ────────
  // La trama está en Flash para no ocupar RAM interna
  // La copiamos a PSRAM para poder procesarla
  Serial.printf("Copiando trama HL7 (%d bytes) de Flash a PSRAM...\n", HL7_TRAMA_LEN);
  memcpy_P(hl7Buffer, HL7_TRAMA, HL7_TRAMA_LEN);
  hl7Buffer[HL7_TRAMA_LEN] = '\0';
  Serial.println("Trama copiada a PSRAM OK");

  // ── Conectar WiFi ──────────────────────────────────────────
  conectarWifi();

  // ── Procesar y enviar ──────────────────────────────────────
  Serial.println("\n>> Procesando trama HL7...");
  procesarTrama();

  Serial.println("\n=== PRUEBA FINALIZADA ===");
  Serial.println("Revisá Firebase Console para ver el documento creado.");
}


// ============================================================
//  LOOP — no hace nada en modo prueba
// ============================================================
void loop() {
  delay(10000);
}


// ============================================================
//  PROCESAR TRAMA HL7
// ============================================================
void procesarTrama() {
  String paciente = extraerCampo("PID", 5);
  String fecha    = extraerCampo("OBR", 7);
  String ecgId    = extraerCampo("OBR", 3);
  paciente.replace("^", "_");

  Serial.printf("Paciente : %s\n", paciente.c_str());
  Serial.printf("ECG ID   : %s\n", ecgId.c_str());
  Serial.printf("Fecha    : %s\n", fecha.c_str());

  char* pdfBase64 = extraerBase64PDF(hl7Buffer);
  if (pdfBase64 == nullptr) {
    Serial.println("ERROR: PDF no encontrado en trama.");
    return;
  }

  Serial.printf("Base64 extraído: %d caracteres\n", strlen(pdfBase64));
  enviarAFirebase(pdfBase64, paciente, ecgId, fecha);
}


// ============================================================
//  EXTRAER BASE64 DEL PDF
//  Busca "JVBERi0" (firma universal de PDF en Base64)
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
//  EXTRAER CAMPO DE SEGMENTO HL7
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
//  ENVIAR A FIRESTORE VIA WiFi
//  El pdfBase64 viaja directo desde PSRAM sin copiar a RAM
// ============================================================
void enviarAFirebase(char* pdfBase64, String paciente,
                     String ecgId, String fecha) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desconectado, reconectando...");
    conectarWifi();
  }

  Serial.println("Enviando a Firebase Firestore...");

  // Armar el JSON header (sin el Base64 que es grande)
  String jsonHeader = "{\"fields\":{";
  jsonHeader += "\"paciente\":{\"stringValue\":\"" + paciente + "\"},";
  jsonHeader += "\"ecg_id\":{\"stringValue\":\"" + ecgId + "\"},";
  jsonHeader += "\"fecha\":{\"stringValue\":\"" + fecha + "\"},";
  jsonHeader += "\"pdf_base64\":{\"stringValue\":\"";

  String jsonFooter = "\"}}}";

  // Calcular tamaño total
  int totalLen = jsonHeader.length() + strlen(pdfBase64) + jsonFooter.length();
  Serial.printf("Tamaño total JSON: %d bytes\n", totalLen);

  HTTPClient http;
  http.begin(FIREBASE_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Content-Length", String(totalLen));
  http.setTimeout(30000);

  // Mandar el JSON en partes — el Base64 va directo desde PSRAM
  WiFiClient* stream = http.getStreamPtr();
  stream->print(jsonHeader);
  stream->print(pdfBase64);   // char* directo desde PSRAM, sin copiar
  stream->print(jsonFooter);

  int httpCode = http.POST((uint8_t*)nullptr, 0);

  if (httpCode == 200 || httpCode == 201) {
    Serial.println("✓ ECG enviado correctamente a Firestore!");
    Serial.println("Revisá Firebase Console → cardiolink-f186f → ecgs");
  } else {
    Serial.printf("✗ Error HTTP: %d\n", httpCode);
    Serial.println(http.getString());
  }

  http.end();
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
