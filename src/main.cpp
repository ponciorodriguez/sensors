#include <SPI.h>
#include <mcp_can.h>
#include <WiFi.h>
#include <WebServer.h>

#define PIN_CS  5     // Chip Select MCP2515
#define PIN_INT 4     // INT MCP2515
#define PIN_ADC 34    // ADC para sensor o potenciómetro

// Cambia por tus credenciales WiFi
const char* ssid = "calaestancia";
const char* password = "Catieta1";

MCP_CAN CAN(PIN_CS);
WebServer server(80);

float tempC = 0.0;
float tempK = 0.0;

// Página web principal con números grandes y auto-actualización
void handleRoot() {
  String page = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  page += "<title>Temperatura Agua</title>";
  page += "<style>.big{font-size:6em;text-align:center;margin-top:2em;}</style>";
  page += "<script>setInterval(()=>{fetch('/temp').then(r=>r.text()).then(t=>document.getElementById('val').innerText=t);},1000);</script>";
  page += "</head><body>";
  page += "<div class='big'><span id='val'>" + String(tempK,2) + "</span> K</div>";
  page += "</body></html>";
  server.send(200, "text/html", page);
}

// Endpoint para devolver solo el valor de temperatura (para AJAX)
void handleTemp() {
  server.send(200, "text/plain", String(tempK,2));
}

void setup() {
  Serial.begin(115200);

  // Inicializa WiFi
  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectado!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // Inicializa MCP2515
  SPI.begin(18, 19, 23, PIN_CS); // SCK, MISO, MOSI, SS
  if (CAN.begin(MCP_ANY, CAN_250KBPS, MCP_8MHZ) == CAN_OK) {
    Serial.println("CAN MCP2515 inicializado!");
  } else {
    Serial.println("Error inicializando CAN MCP2515!");
    while (1);
  }
  CAN.setMode(MCP_NORMAL);

  pinMode(PIN_INT, INPUT);
  pinMode(PIN_ADC, INPUT);

  // Inicializa WebServer
  server.on("/", handleRoot);
  server.on("/temp", handleTemp);
  server.begin();

  Serial.println("Sketch PGN 130310 listo y web activa!");
}

void loop() {
  // Lee el ADC y calcula temperatura
  int adcRaw = analogRead(PIN_ADC); // 0–4095
  tempC = (adcRaw / 4095.0f) * 100.0f;
  tempK = tempC + 273.15;
  uint16_t tempRaw = (uint16_t)(tempK); // SOLO KELVIN

  // Envío por CAN cada 1 segundo
  static unsigned long lastTx = 0;
  if (millis() - lastTx >= 1000) {
    lastTx = millis();

    Serial.print("ADC raw: "); Serial.print(adcRaw);
    Serial.print(" TempC: "); Serial.print(tempC, 2);
    Serial.print(" TempK: "); Serial.print(tempK, 2);
    Serial.print(" Kelvin: "); Serial.println(tempRaw);

    // PGN 130310: Environmental Parameters
    byte data[8] = {0};
    data[0] = 0x01; // SID
    data[1] = 0x01; // Water temperature channel
    data[2] = tempRaw & 0xFF;         // LSB
    data[3] = (tempRaw >> 8) & 0xFF;  // MSB
    data[4] = 0xFF; // Reserved
    data[5] = 0xFF; // Reserved
    data[6] = 0xFF; // Reserved
    data[7] = 0xFF; // Reserved

    unsigned long pgn = 130310;
    unsigned long extId = (6UL << 26) | (pgn << 8) | 1;

    CAN.sendMsgBuf(extId, 1, 8, data);
  }

  server.handleClient(); // Atiende peticiones web
}