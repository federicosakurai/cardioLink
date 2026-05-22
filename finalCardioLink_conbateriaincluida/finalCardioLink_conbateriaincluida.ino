#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// ============================================================
// WIFI
// ============================================================

const char* WIFI_SSID = "TeleCentro-78e2";
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
#define SD_SCK 14
#define SD_CS 13
#define LED_ROJO 25
#define LED_AMARILLO 26
#define LED_VERDE1 27
#define LED_VERDE2 32
#define LED_VERDE3 33

// ============================================================
// BATERIA
// ============================================================

#define BATTERY_PIN 35

TaskHandle_t TaskBateria;

// ============================================================
// BUFFER EN PSRAM
// ============================================================

#define BUFFER_SIZE (200 * 1024)

char* hl7Buffer = nullptr;

// ============================================================
// TASK BATERIA
// ============================================================

void taskBateria(void* parameter) {

  while (true) {

    // ======================================================
    // LEER ADC
    // ======================================================

    int raw = analogRead(BATTERY_PIN);

    // ======================================================
    // CALCULAR VOLTAJE
    // ======================================================

    float voltaje =
      ((float)raw / 4095.0) * 2.0 * 3.3 * 1.1;

    // ======================================================
    // CALCULAR PORCENTAJE
    // ======================================================

    int porcentaje =
      map(voltaje * 100,
          330,
          420,
          0,
          100);

    porcentaje = constrain(porcentaje, 0, 100);

    // ======================================================
    // MOSTRAR
    // ======================================================

    Serial.printf("Bateria: %d%% | %.2fV\n",
                  porcentaje,
                  voltaje);

    // ======================================================
    // APAGAR TODOS
    // ======================================================

    digitalWrite(LED_ROJO, LOW);
    digitalWrite(LED_AMARILLO, LOW);
    digitalWrite(LED_VERDE1, LOW);
    digitalWrite(LED_VERDE2, LOW);
    digitalWrite(LED_VERDE3, LOW);

    // ======================================================
    // ENCENDER SEGUN %
    // ======================================================

    if (porcentaje > 0)
      digitalWrite(LED_ROJO, HIGH);

    if (porcentaje >= 20)
      digitalWrite(LED_AMARILLO, HIGH);

    if (porcentaje >= 40)
      digitalWrite(LED_VERDE1, HIGH);

    if (porcentaje >= 60)
      digitalWrite(LED_VERDE2, HIGH);

    if (porcentaje >= 80)
      digitalWrite(LED_VERDE3, HIGH);

    // ======================================================
    // ESPERAR
    // ======================================================

    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(2000);

  // ==========================================================
  // CONFIGURAR LEDS
  // ==========================================================

  pinMode(LED_ROJO, OUTPUT);
  pinMode(LED_AMARILLO, OUTPUT);
  pinMode(LED_VERDE1, OUTPUT);
  pinMode(LED_VERDE2, OUTPUT);
  pinMode(LED_VERDE3, OUTPUT);

  // ==========================================================
  // TASK BATERIA EN CORE 0
  // ==========================================================

  xTaskCreatePinnedToCore(
    taskBateria,
    "TaskBateria",
    4000,
    NULL,
    1,
    &TaskBateria,
    0);

  Serial.println("\n=== PULSONET — MULTI ECG ===");

  // ==========================================================
  // PSRAM
  // ==========================================================

  if (!psramFound()) {

    Serial.println("ERROR: PSRAM no detectada");

    while (true) delay(1000);
  }

  hl7Buffer = (char*)ps_malloc(BUFFER_SIZE);

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
  // WIFI
  // ==========================================================

  conectarWifi();

  Serial.println("\nSistema listo.");
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // ==========================================================
  // RECIBIR NUEVO HL7
  // ==========================================================

  recibirHL7Serial();

  // ==========================================================
  // CARGAR EN PSRAM
  // ==========================================================

  cargarHL7DesdeSD();

  // ==========================================================
  // PROCESAR
  // ==========================================================

  procesarTrama();

  Serial.println("\n=================================");
  Serial.println("Listo para recibir otro ECG");
  Serial.println("=================================\n");

  delay(1000);
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

    return;
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

  while (*fin != '\0' && *fin != '|' && *fin != '\r' && *fin != '\n' && *fin != 0x1C) {

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

  char temp[256] = { 0 };

  int i = 0;

  while (*inicio && *inicio != '\r' && *inicio != '\n') {

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
    String(FIREBASE_URL) + "?documentId=" + docId;

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

  int httpCode = http.POST(json);

  if (httpCode > 0) {

    Serial.printf("HTTP Code: %d\n", httpCode);

    String response = http.getString();

    Serial.println(response);

    if (httpCode == 200 || httpCode == 201) {

      Serial.println("✓ ECG enviado correctamente!");
      Serial.println("Documento: " + docId);
    }
  } else {

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

  while (WiFi.status() != WL_CONNECTED && intentos < 20) {

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
  } else {

    Serial.println("\nERROR WiFi");
  }
}
