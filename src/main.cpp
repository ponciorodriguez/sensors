#include <SPI.h>
#include <mcp_can.h>

#define PIN_CS  5
#define PIN_INT 4
#define PIN_ADC 34

MCP_CAN CAN(PIN_CS);

void setup() {
  Serial.begin(115200);
  SPI.begin(18, 19, 23, PIN_CS);
  CAN.begin(MCP_ANY, CAN_250KBPS, MCP_8MHZ);
  CAN.setMode(MCP_NORMAL);
  pinMode(PIN_INT, INPUT);
  pinMode(PIN_ADC, INPUT);
  Serial.println("Sketch PGN 130310 listo!");
}

void loop() {
  static unsigned long lastTx = 0;
  if (millis() - lastTx >= 1000) {
    lastTx = millis();
    int adcRaw = analogRead(PIN_ADC); // 0–4095
    float tempC = (adcRaw / 4095.0f) * 100.0f;
    float tempK = tempC + 273.15;
    uint16_t tempRaw = (uint16_t)(tempK); // SOLO KELVIN

    Serial.print("ADC raw: "); Serial.print(adcRaw);
    Serial.print(" TempC: "); Serial.print(tempC, 2);
    Serial.print(" TempK: "); Serial.print(tempK, 2);
    Serial.print(" Kelvin: "); Serial.println(tempRaw);

    // PGN 130310: Environmental Parameters
    byte data[8] = {0};
    data[0] = 0x01; // SID
    data[1] = 0x02; // Outside air (puedes cambiar por 0x01 para agua)
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
}