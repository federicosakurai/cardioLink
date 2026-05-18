#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// ============================================================
// WIFI
// ============================================================

const char* WIFI_SSID     = "TeleCentro-78e2";
const char* WIFI_PASSWORD = "EZM2MGYMGZMN";

// ============================================================
// FIREBASE
// ============================================================

const char* FIREBASE_URL =
"https://firestore.googleapis.com/v1/projects/cardiolink-f186f/databases/(default)/documents/ecgs";

// ============================================================
// SD — LILYGO T-A7670E
// ============================================================

#define SD_MISO 2
#define SD_MOSI 15
#define SD_SCK  14
#define SD_CS   13

// ============================================================
// BUFFER EN PSRAM
// ============================================================

#define BUFFER_SIZE (200 * 1024)

char* hl7Buffer = nullptr;

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(2000);

  Serial.println("\n=== PULSONET — SERIAL → SD → PSRAM ===");

  // ==========================================================
  // PSRAM
  // ==========================================================

  if (!psramFound()) {

    Serial.println("ERROR: PSRAM no detectada");

    while (true) delay(1000);
  }

  hl7Buffer = (char*) ps_malloc(BUFFER_SIZE);

  if (hl7Buffer == nullptr) {

    Serial.println("ERROR: No se pudo alocar PSRAM");

    while (true) delay(1000);
  }

  Serial.printf("PSRAM OK — %d KB\n", BUFFER_SIZE / 1024);

  // ==========================================================
  // SPI + SD
  // ==========================================================

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS)) {

    Serial.println("ERROR: SD no detectada");

    while (true) delay(1000);
  }

  Serial.println("SD OK");

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);

  Serial.print("Tamano SD: ");
  Serial.print(cardSize);
  Serial.println(" MB");

  // ==========================================================
  // RECIBIR SERIAL → SD
  // ==========================================================

  recibirHL7Serial();

  // ==========================================================
  // LEER SD → PSRAM
  // ==========================================================

  cargarHL7DesdeSD();

  // ==========================================================
  // WIFI
  // ==========================================================

  conectarWifi();

  // ==========================================================
  // PROCESAR HL7
  // ==========================================================

  procesarTrama();

  Serial.println("\n=== FINALIZADO ===");
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  delay(10000);
}

// ============================================================
// RECIBIR HL7 DESDE SERIAL
// ============================================================

void recibirHL7Serial() {

  Serial.println("\nEsperando HL7 por Serial...");
  Serial.println("Pegar HL7 completo");
  Serial.println("Finaliza automaticamente");

  SD.remove("/trama.hl7");

  File archivo = SD.open("/trama.hl7", FILE_WRITE);

  if (!archivo) {

    Serial.println("ERROR creando archivo");

    while (true) delay(1000);
  }

  // ==========================================================
  // ESPERAR PRIMER DATO
  // ==========================================================

  while (!Serial.available()) {

    delay(10);
  }

  Serial.println("Recibiendo datos...");

  unsigned long ultimoDato = millis();

  // ==========================================================
  // RECIBIR DATOS
  // ==========================================================

  while (true) {

    while (Serial.available()) {

      char c = Serial.read();

      archivo.write(c);

      ultimoDato = millis();
    }

    // ======================================================
    // TIMEOUT = FIN AUTOMATICO
    // ======================================================

    if (millis() - ultimoDato > 3000) {

      archivo.close();

      Serial.println("HL7 guardado en SD");

      return;
    }
  }
}

// ============================================================
// CARGAR SD → PSRAM
// ============================================================

void cargarHL7DesdeSD() {

  File archivo = SD.open("/trama.hl7");

  if (!archivo) {

    Serial.println("ERROR abriendo archivo");

    while (true) delay(1000);
  }

  int bytesLeidos =
      archivo.readBytes(hl7Buffer, BUFFER_SIZE - 1);

  hl7Buffer[bytesLeidos] = '\0';

  archivo.close();

  Serial.printf("HL7 cargado en PSRAM: %d bytes\n",
                bytesLeidos);
}

// ============================================================
// PROCESAR TRAMA HL7
// ============================================================

void procesarTrama() {

  String paciente = extraerCampo("PID", 5);

  String fecha = extraerCampo("OBR", 7);

  String ecgId = extraerCampo("OBR", 3);

  paciente.replace("^", "_");

  Serial.printf("Paciente : %s\n", paciente.c_str());

  Serial.printf("ECG ID   : %s\n", ecgId.c_str());

  Serial.printf("Fecha    : %s\n", fecha.c_str());

  char* pdfBase64 = extraerBase64PDF(hl7Buffer);

  if (pdfBase64 == nullptr) {

    Serial.println("ERROR: PDF no encontrado");

    return;
  }

  Serial.printf("Base64 extraido: %d caracteres\n",
                strlen(pdfBase64));

  enviarAFirebase(pdfBase64,
                   paciente,
                   ecgId,
                   fecha);
}

// ============================================================
// EXTRAER PDF BASE64
// ============================================================

char* extraerBase64PDF(char* hl7) {

  char* inicio = strstr(hl7, "JVBERi0");

  if (inicio == nullptr) {

    Serial.println("Firma PDF no encontrada");

    return nullptr;
  }

  char* fin = inicio;

  while (*fin != '\0' &&
         *fin != '|' &&
         *fin != '\r' &&
         *fin != '\n' &&
         *fin != 0x1C) {

    fin++;
  }

  *fin = '\0';

  return inicio;
}

// ============================================================
// EXTRAER CAMPO HL7
// ============================================================

String extraerCampo(const char* segmento,
                    int campoNum) {

  char patron[10];

  sprintf(patron, "%s|", segmento);

  char* inicio = strstr(hl7Buffer, patron);

  if (inicio == nullptr) {

    return "DESCONOCIDO";
  }

  inicio += strlen(patron);

  int campoActual = 1;

  char temp[256] = {0};

  int i = 0;

  while (*inicio &&
         *inicio != '\r' &&
         *inicio != '\n') {

    if (*inicio == '|') {

      campoActual++;

      inicio++;

      continue;
    }

    if (campoActual == campoNum) {

      temp[i++] = *inicio;

      if (i >= 255) break;
    }

    inicio++;
  }

  temp[i] = '\0';

  if (strlen(temp) == 0) {

    return "DESCONOCIDO";
  }

  return String(temp);
}

// ============================================================
// ENVIAR A FIREBASE
// ============================================================

void enviarAFirebase(char* pdfBase64,
                     String paciente,
                     String ecgId,
                     String fecha) {

  if (WiFi.status() != WL_CONNECTED) {

    conectarWifi();
  }

  Serial.println("Enviando a Firestore...");

  // ==========================================================
  // CREAR ID DOCUMENTO
  // ==========================================================

  String docId = paciente + "_" + ecgId;

  docId.replace(" ", "_");
  docId.replace("^", "_");
  docId.replace("/", "_");
  docId.replace("\\", "_");

  // ==========================================================
  // URL FINAL
  // ==========================================================

  String url =
      String(FIREBASE_URL) +
      "?documentId=" +
      docId;

  // ==========================================================
  // CREAR JSON
  // ==========================================================

  String json =
      "{\"fields\":{";

  json += "\"paciente\":{\"stringValue\":\"";
  json += paciente;
  json += "\"},";

  json += "\"ecg_id\":{\"stringValue\":\"";
  json += ecgId;
  json += "\"},";

  json += "\"fecha\":{\"stringValue\":\"";
  json += fecha;
  json += "\"},";

  json += "\"pdf_base64\":{\"stringValue\":\"";
  json += String(pdfBase64);
  json += "\"}";

  json += "}}";

  Serial.printf("Tamano JSON: %d bytes\n",
                json.length());

  HTTPClient http;

  http.begin(url);

  http.addHeader("Content-Type",
                 "application/json");

  http.setTimeout(60000);

  // ==========================================================
  // POST
  // ==========================================================

  int httpCode = http.POST(json);

  if (httpCode > 0) {

    Serial.printf("HTTP Code: %d\n", httpCode);

    String response = http.getString();

    Serial.println(response);

    if (httpCode == 200 ||
        httpCode == 201) {

      Serial.println("✓ ECG enviado correctamente!");
      Serial.println("Documento: " + docId);
    }
  }
  else {

    Serial.printf("ERROR HTTP: %s\n",
                  http.errorToString(httpCode).c_str());
  }

  http.end();
}

// ============================================================
// WIFI
// ============================================================

void conectarWifi() {

  Serial.printf("Conectando WiFi: %s",
                WIFI_SSID);

  WiFi.begin(WIFI_SSID,
             WIFI_PASSWORD);

  int intentos = 0;

  while (WiFi.status() != WL_CONNECTED &&
         intentos < 20) {

    delay(500);

    Serial.print(".");

    intentos++;
  }

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println("\nWiFi conectado");

    Serial.print("IP: ");

    Serial.println(WiFi.localIP());

    Serial.printf("Heap libre: %d\n",
                  ESP.getFreeHeap());
  }
  else {

    Serial.println("\nERROR WiFi");
  }
}