#include <SPI.h>
#include <mcp_can.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

// =============== Pines / HW ======================
#define PIN_CS       5
#define PIN_INT      4
#define PIN_ADC      34
#define PIN_PARADA   27
#define PIN_NAVLIGHT 26
#define PIN_BATTERY  35

// =============== WiFi ============================
const char* ssid = "calaestancia";
const char* password = "Catieta1";
IPAddress local_IP(192, 168, 1, 63);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(192, 168, 1, 1);

// =============== CAN =============================
MCP_CAN CAN(PIN_CS);

static const uint8_t N2K_PRIO_RAPID = 2;
static const uint8_t N2K_PRIO_DYN   = 2;
static const uint8_t N2K_PRIO_MISC  = 6;
static const uint8_t N2K_SA = 0xA5;

// =============== Web/UI ==========================
WebServer server(80);

float tempC = 0.0f;
float batteryV = 0.0f;
float potValue = 3000.0f; // Valor óptimo, ajustable desde web si quieres
Preferences prefs; // Para guardar en flash
const int rpmSim = 2500;
volatile bool motorParado = false;
volatile bool navLightOn = false;

// =============== Interpolación lineal ===============
// A 60ºC hay 1.00V en ADC. A 120ºC hay 0.67V en ADC.
float V1 = 1.00; // Voltaje a 60 ºC
float V2 = 0.67; // Voltaje a 120 ºC
float temp1 = 60.0; // Temperatura 1
float temp2 = 120.0; // Temperatura 2

float readTempLinear(int adcRaw) {
  float Vadc = adcRaw * 3.3f / 4095.0f;
  return temp1 + (V1 - Vadc) * (temp2 - temp1) / (V1 - V2);
}

// =============== Helpers N2K =====================
static uint8_t fpSeq127489 = 0;
static inline uint16_t encodeTemp_dK(float celsius) {
  float raw = (celsius + 273.15f) * 10.0f + 0.5f;
  if (raw <= 0.0f || raw >= 65535.0f) return 0xFFFF;
  return (uint16_t)raw;
}

void sendPGN60928_AddressClaim() {
  const uint32_t PGN = 60928UL;
  uint32_t extId = ((uint32_t)N2K_PRIO_MISC << 26) | (PGN << 8) | N2K_SA;
  uint8_t name[8] = { 0x01,0x23,0x45,0x67, 0x89,0xAB, 0xCD,0xEF };
  CAN.sendMsgBuf(extId, 1, 8, name);
  Serial.println("[N2K 60928] Address Claim enviado (SA=0xA5).");
}

void sendPGN127488(uint8_t engineInstance, uint16_t rpm) {
  uint8_t data[8];
  uint16_t rpmRaw = (uint16_t)(rpm * 4U);

  data[0] = engineInstance;
  data[1] = rpmRaw & 0xFF;
  data[2] = (rpmRaw >> 8) & 0xFF;
  data[3] = 0xFF;
  data[4] = 0xFF;
  data[5] = 0xFF;
  data[6] = 0xFF;
  data[7] = 0xFF;

  const uint32_t PGN = 127488UL;
  uint32_t extId = ((uint32_t)N2K_PRIO_RAPID << 26) | (PGN << 8) | N2K_SA;

  CAN.sendMsgBuf(extId, 1, 8, data);
}

void sendPGN127489(uint8_t engineInstance, float tempCelsius, float battV) {
  uint8_t p[26];
  memset(p, 0xFF, sizeof(p));
  p[0] = engineInstance; // Instance 0

  uint16_t t_dK = encodeTemp_dK(tempCelsius);

  p[3] = t_dK & 0xFF;
  p[4] = (t_dK >> 8) & 0xFF;
  p[5] = t_dK & 0xFF;
  p[6] = (t_dK >> 8) & 0xFF;

  if (battV > 0.0f && battV < 100.0f) {
    uint16_t alt_dV = (uint16_t)lroundf(battV * 100.0f);
    p[7] = alt_dV & 0xFF;
    p[8] = (alt_dV >> 8) & 0xFF;
  }

  p[20] = 0x00; p[21] = 0x00;
  p[22] = 0x00; p[23] = 0x00;

  const uint32_t PGN = 127489UL;
  uint32_t extId = ((uint32_t)N2K_PRIO_DYN << 26) | (PGN << 8) | N2K_SA;

  uint8_t f0[8], f1[8], f2[8], f3[8];

  f0[0] = (uint8_t)((fpSeq127489 << 5) | 0);
  f0[1] = 26;
  memcpy(&f0[2], &p[0], 6);

  f1[0] = (uint8_t)((fpSeq127489 << 5) | 1);
  memcpy(&f1[1], &p[6], 7);

  f2[0] = (uint8_t)((fpSeq127489 << 5) | 2);
  memcpy(&f2[1], &p[13], 7);

  f3[0] = (uint8_t)((fpSeq127489 << 5) | 3);
  memcpy(&f3[1], &p[20], 6);
  f3[7] = 0xFF;

  CAN.sendMsgBuf(extId, 1, 8, f0);
  CAN.sendMsgBuf(extId, 1, 8, f1);
  CAN.sendMsgBuf(extId, 1, 8, f2);
  CAN.sendMsgBuf(extId, 1, 8, f3);

  fpSeq127489 = (fpSeq127489 + 1) & 0x07;
}

// =============== Web server =======================
void handleRoot() {
  String page = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  page += "<title>Panel Motor</title>";
  page += "<script src='https://cdn.jsdelivr.net/npm/canvas-gauges@2.1.7/gauge.min.js'></script>";
  page += "<style>html,body{height:100%;} body{background:#111;margin:0;height:100vh;display:flex;flex-direction:column;justify-content:flex-start;align-items:center;}";
  page += ".title{color:#fff;font-size:2em;text-align:center;margin-top:1em;}";
  page += ".gauges{display:flex;justify-content:center;align-items:center;gap:3em;margin-top:3em;}";
  page += ".gauge{background:#222;padding:1em;border-radius:1em;display:flex;flex-direction:column;align-items:center;}";
  page += ".gauge canvas{display:block;width:150px;height:150px;}";
  page += ".gauge-value{color:#fff;font-size:1.1em;margin-top:0.4em;text-align:center;}";
  page += ".btn-parada, .btn-navlight{position:fixed;left:50%;transform:translateX(-50%);z-index:100;}";
  page += ".btn-parada{bottom:1cm;padding:1em 2em;font-size:1.5em;background:#c00;color:#fff;border:none;border-radius:1em;cursor:pointer;}";
  page += ".btn-parada.stop{background:#c00;} .btn-parada.run{background:#090;}";
  page += ".btn-navlight{bottom:6.5em;padding:1em 2em;font-size:1.3em;background:#009;color:#fff;border:none;border-radius:1em;cursor:pointer;}";
  page += ".btn-navlight.on{background:#090;} .btn-navlight.off{background:#009;}";
  page += "#alarm-temp{color:#fff; background:#c00; font-size:2em; padding:1em; border-radius:1em; position:fixed; top:2em; left:50%; transform:translateX(-50%); z-index:9999; text-align:center; box-shadow:0 0 20px #c00;}";
  page += ".calib-toggle-btn{margin-top:2em; font-size:1.2em; background:#090; color:#fff; border:none; border-radius:0.7em; padding:0.7em 2em; cursor:pointer; z-index:10001;}";
  page += ".calib-pot{color:#fff; background:#222; padding:1em; border-radius:0.7em; box-shadow:0 0 15px #000; font-size:1.2em; text-align:center; position:fixed; left:50%; top:15%; transform:translateX(-50%); z-index:10000; width:320px;}";
  page += ".calib-pot input{font-size:1em; width:5em;}";
  page += ".calib-pot button{font-size:1em; margin-left:1em; background:#090; color:#fff; border:none; border-radius:0.4em; padding:0.2em 1em; cursor:pointer;}";
  page += "</style></head><body>";
  page += "<div class='title'>MOTOR</div>";
  page += "<div class='gauges'>";
  page += "<div class='gauge'><canvas id='gauge-temp' width='150' height='150'></canvas><div class='gauge-value' id='val-temp'>---</div><div style='text-align:center;color:#fff;'>Temperatura (&deg;C)</div></div>";
  page += "<div class='gauge'><canvas id='gauge-rpm' width='150' height='150'></canvas><div class='gauge-value' id='val-rpm'>---</div><div style='text-align:center;color:#fff;'>RPM</div></div>";
  page += "<div class='gauge'><canvas id='gauge-bat' width='150' height='150'></canvas><div class='gauge-value' id='val-bat'>---</div><div style='text-align:center;color:#fff;'>Batería (V)</div></div>";
  page += "</div>";
  page += "<button id='show-pot-calib' class='calib-toggle-btn'>Calibrar potenciómetro</button>";
  page += "<div class='calib-pot' id='calib-pot-block' style='display:none; position:fixed; left:50%; top:15%; transform:translateX(-50%); z-index:10000; width:320px;'>";
  page += "Valor del potenciómetro (ohmios): <input id='potval' type='number' min='1000' max='20000' step='100' value='" + String(potValue,0) + "'>";
  page += "<button onclick='setPot()'>Guardar</button>";
  page += "<span id='potstatus'></span>";
  page += "<br>";
  page += "<button onclick='hidePotCalib()' style=\"margin-top:1em;background:#444;color:#fff;\">Cerrar ajuste</button>";
  page += "</div>";
  page += "<button id='btn-navlight' class='btn-navlight off'>LUZ NAVEGACIÓN OFF</button>";
  page += "<button id='btn-parada' class='btn-parada stop'>PARADA EMERGENCIA</button>";
  page += "<script>";
  page += "let gaugeTemp, gaugeRPM, gaugeBat, parado=false, navlight=false;";
  page += "let potInput = null;";
  page += "function setPot(){";
  page += "let v = parseInt(document.getElementById('potval').value);";
  page += "fetch('/potval?valor='+v).then(r=>r.json()).then(j=>{";
  page += "document.getElementById('potstatus').textContent = 'Potenciómetro = ' + j.pot + ' Ω';";
  page += "});}";
  page += "function hidePotCalib(){";
  page += "document.getElementById('calib-pot-block').style.display = 'none';";
  page += "}";
  page += "function updateBtn(){ const btn=document.getElementById('btn-parada'); if(parado){btn.textContent='REARMAR MOTOR';btn.className='btn-parada run';}else{btn.textContent='PARADA EMERGENCIA';btn.className='btn-parada stop';}}";
  page += "function updateNavBtn(){ const btn=document.getElementById('btn-navlight'); if(navlight){btn.textContent='LUZ NAVEGACIÓN ON';btn.className='btn-navlight on';}else{btn.textContent='LUZ NAVEGACIÓN OFF';btn.className='btn-navlight off';}}";
  page += "document.addEventListener('DOMContentLoaded',function(){";
  page += "gaugeTemp = new RadialGauge({renderTo:'gauge-temp',minValue:0,maxValue:120,units:'°C',majorTicks:[0,20,40,60,80,100,120],minorTicks:4,highlights:[{from:90,to:120,color:'rgba(200,50,50,.75)'}],value:0,width:150,height:150}).draw();";
  page += "gaugeRPM = new RadialGauge({renderTo:'gauge-rpm',minValue:0,maxValue:4000,units:'rpm',majorTicks:[0,500,1000,1500,2000,2500,3000,3500,4000],minorTicks:4,highlights:[{from:3500,to:4000,color:'rgba(200,50,50,.75)'}],value:0,width:150,height:150}).draw();";
  page += "gaugeBat = new RadialGauge({renderTo:'gauge-bat',minValue:10,maxValue:15,units:'V',majorTicks:[10,11,12,13,14,15],minorTicks:5,highlights:[{from:10,to:11.5,color:'rgba(200,50,50,.75)'},{from:14.5,to:15,color:'rgba(200,200,50,.75)'}],value:0,width:150,height:150}).draw();";
  page += "potInput = document.getElementById('potval');";
  page += "document.getElementById('show-pot-calib').onclick = function(){ document.getElementById('calib-pot-block').style.display = 'block'; };";
  page += "setInterval(()=>{fetch('/vals').then(r=>r.json()).then(j=>{";
  page += "gaugeTemp.value = j.temp; document.getElementById('val-temp').textContent = j.temp.toFixed(1);";
  page += "gaugeRPM.value = j.rpm; document.getElementById('val-rpm').textContent = j.rpm;";
  page += "gaugeBat.value = j.bat; document.getElementById('val-bat').textContent = j.bat.toFixed(2);";
  page += "parado = j.parado; updateBtn(); navlight = j.navlight; updateNavBtn();";
  page += "if (document.activeElement !== potInput) { potInput.value = j.pot; }";
  page += "document.getElementById('potstatus').textContent = 'Potenciómetro = ' + j.pot + ' Ω';";
  page += "if (j.temp > 110) { if (!document.getElementById('alarm-temp')) { let alarm = document.createElement('div'); alarm.id = 'alarm-temp'; alarm.innerHTML = '&#9888; <b>PELIGRO:</b> Temp motor muy alta (' + j.temp.toFixed(1) + ' °C)'; document.body.appendChild(alarm);} } else { let alarm = document.getElementById('alarm-temp'); if (alarm) alarm.remove(); }";
  page += "});},500);";
  page += "document.getElementById('btn-parada').onclick=function(){ parado = !parado; updateBtn(); fetch('/parada',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({parar:parado})}).then(()=>{}); };";
  page += "document.getElementById('btn-navlight').onclick=function(){ navlight = !navlight; updateNavBtn(); fetch('/navlight',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({on:navlight})}).then(()=>{}); };";
  page += "});";
  page += "</script></body></html>";
  server.send(200, "text/html", page);
}

void handleVals() {
  String json = "{";
  json += "\"temp\":" + String(tempC, 1) + ",";
  json += "\"rpm\":" + String(rpmSim) + ",";
  json += "\"bat\":" + String(batteryV, 2) + ",";
  json += "\"parado\":" + String(motorParado ? "true" : "false") + ",";
  json += "\"navlight\":" + String(navLightOn ? "true" : "false") + ",";
  json += "\"pot\":" + String(potValue, 0);
  json += "}";
  server.send(200, "application/json", json);
}

void handlePotVal() {
  if (server.hasArg("valor")) {
    float v = server.arg("valor").toFloat();
    if (v >= 1000.0f && v <= 20000.0f) {
      potValue = v;
      prefs.begin("calib", false);
      prefs.putFloat("potValue", potValue);
      prefs.end();
    }
    server.send(200, "application/json", "{\"ok\":true,\"pot\":" + String(potValue,0) + "}");
  } else server.send(400, "application/json", "{\"ok\":false}");
}

void handleParada() {
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    bool parar = body.indexOf("true") > 0 || body.indexOf("parar\":true") > 0;
    motorParado = parar;
    digitalWrite(PIN_PARADA, motorParado ? HIGH : LOW);
    server.send(200, "application/json", "{\"ok\":true}");
  } else server.send(400, "application/json", "{\"ok\":false}");
}

void handleNavLight() {
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    bool turnOn = body.indexOf("true") > 0 || body.indexOf("on\":true") > 0;
    navLightOn = turnOn;
    digitalWrite(PIN_NAVLIGHT, navLightOn ? HIGH : LOW);
    server.send(200, "application/json", "{\"ok\":true}");
  } else server.send(400, "application/json", "{\"ok\":false}");
}

void setup() {
  Serial.begin(115200);

  WiFi.config(local_IP, gateway, subnet, dns);
  WiFi.begin(ssid, password);
  Serial.println("Conectando a WiFi...");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println(); Serial.print("WiFi conectado. IP: "); Serial.println(WiFi.localIP());

  SPI.begin(18, 19, 23, PIN_CS);
  if (CAN.begin(MCP_ANY, CAN_250KBPS, MCP_8MHZ) != CAN_OK) {
    Serial.println("Error inicializando MCP_CAN");
    while (1) delay(100);
  }
  CAN.setMode(MCP_NORMAL);

  pinMode(PIN_INT, INPUT);
  pinMode(PIN_ADC, INPUT);
  pinMode(PIN_BATTERY, INPUT);
  pinMode(PIN_PARADA, OUTPUT);   digitalWrite(PIN_PARADA, LOW);
  pinMode(PIN_NAVLIGHT, OUTPUT); digitalWrite(PIN_NAVLIGHT, LOW);

  prefs.begin("calib", false);
  potValue = prefs.getFloat("potValue", 3000.0f);
  prefs.end();

  server.on("/", handleRoot);
  server.on("/vals", handleVals);
  server.on("/parada", HTTP_POST, handleParada);
  server.on("/navlight", HTTP_POST, handleNavLight);
  server.on("/potval", handlePotVal);

  server.begin();
  Serial.println("Servidor web iniciado.");

  sendPGN60928_AddressClaim();
}

void loop() {
  int adcRaw = analogRead(PIN_ADC);

  // Interpolación lineal entre voltaje y temperatura
  float Vadc = adcRaw * 3.3f / 4095.0f;
  tempC = temp1 + (V1 - Vadc) * (temp2 - temp1) / (V1 - V2);

  int adcBat = analogRead(PIN_BATTERY);
  batteryV = adcBat * (16.0f / 4095.0f);

  static unsigned long lastTxRPM = 0;
  if (millis() - lastTxRPM >= 500) {
    lastTxRPM = millis();
    sendPGN127488(0, rpmSim);
  }

  static unsigned long lastTx316 = 0;
  if (millis() - lastTx316 >= 1000) {
    lastTx316 = millis();
    byte data_130316[8] = {0};
    data_130316[0] = 0x00;
    data_130316[1] = 0x00;
    data_130316[2] = 0x16;

    uint32_t temp_mK = (uint32_t)lroundf((tempC + 273.15f) * 1000.0f);
    if (temp_mK > 0xFFFFFE) temp_mK = 0xFFFFFE;
    data_130316[3] = (uint8_t)(temp_mK & 0xFF);
    data_130316[4] = (uint8_t)((temp_mK >> 8) & 0xFF);
    data_130316[5] = (uint8_t)((temp_mK >> 16) & 0xFF);

    data_130316[6] = 0xFF;
    data_130316[7] = 0xFF;

    const uint32_t PGN = 130316UL;
    uint32_t extId = ((uint32_t)N2K_PRIO_MISC << 26) | (PGN << 8) | N2K_SA;

    CAN.sendMsgBuf(extId, 1, 8, data_130316);
  }

  static unsigned long lastTx489 = 0;
  if (millis() - lastTx489 >= 1000) {
    lastTx489 = millis();
    sendPGN127489(0, tempC, batteryV);
  }

  server.handleClient();
}