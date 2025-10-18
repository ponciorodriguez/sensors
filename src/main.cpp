#include <SPI.h>
#include <mcp_can.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

// =================== CONFIG SENSOR ===================
// true -> NTC 10k en divisor (R fija = potValue). false -> tu interpolación lineal V1/V2
#define USE_NTC true

// --- Calibración lineal (T_real = A*T_meas + B) ---
#define ENABLE_TCAL true
static const float T_CAL_A = 1.3043478f;   // pendiente (de 30->25 y 87.5->100)
static const float T_CAL_B = -14.1304348f; // offset

// (opcional) límites de seguridad para clamp
#define ENABLE_TCLAMP true
static const float T_MIN_C = -20.0f;
static const float T_MAX_C = 150.0f;

// Parámetros NTC (si USE_NTC == true)
static const float NTC_R0  = 10000.0f;          // 10k @ 25°C
static const float NTC_BETA= 3950.0f;           // ajusta si tu NTC es 3435/4200...
static const float NTC_T0K = 273.15f + 25.0f;   // 298.15 K

// Si usas sensor lineal (tu caso original):
static const float V1 = 1.00f;   // Voltaje a 60 ºC
static const float V2 = 0.67f;   // Voltaje a 120 ºC
static const float TEMP1 = 60.0f;
static const float TEMP2 = 120.0f;

// ADC
static const float ADC_VREF = 3.3f;
static const int   ADC_NSAMPLES = 16;  // media para suavizar

// Batería: configura tu divisor real: Vbat -> [Rtop]---(nodo)---[Rbottom] -> GND
static const float VBAT_RTOP   = 100000.0f;  // ohmios
static const float VBAT_RBOT   = 22000.0f;   // ohmios
static const float VBAT_GAIN   = (VBAT_RTOP + VBAT_RBOT) / VBAT_RBOT;

// ================= Pines / HW ======================
#define PIN_CS       5
#define PIN_INT      4
#define PIN_ADC      34
#define PIN_PARADA   27
#define PIN_NAVLIGHT 26
#define PIN_BATTERY  35

// ================= WiFi ============================
const char* ssid = "calaestancia";
const char* password = "Catieta1";
IPAddress local_IP(192, 168, 1, 63);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(192, 168, 1, 1);

// ================= Signal K (UDP Delta) ============
WiFiUDP skUDP;
const char* SK_UDP_HOST = "192.168.1.50";   // IP de tu OpenPlotter/Signal K
const uint16_t SK_UDP_PORT = 4123;          // Puerto UDP Delta (actívalo en Signal K)
const char* SK_PATH_COOLANT = "propulsion.main.coolantTemperature";

// ================= CAN / N2K =======================
MCP_CAN CAN(PIN_CS);

static const uint8_t N2K_PRIO_RAPID = 2;
static const uint8_t N2K_PRIO_DYN   = 2;
static const uint8_t N2K_PRIO_MISC  = 6;
static const uint8_t N2K_SA = 0xA5;

// ================= Web/UI ==========================
WebServer server(80);

float tempC = 0.0f;
float batteryV = 0.0f;
float potValue = 3000.0f; // R fija del divisor NTC si USE_NTC=true
Preferences prefs;
const int rpmSim = 2500;
volatile bool motorParado = false;
volatile bool navLightOn = false;

// ================= Helpers N2K =====================
static uint8_t fpSeq127489 = 0;
static inline uint16_t encodeTemp_dK(float celsius) {
  // 0.1 K por unidad; 0xFFFF = Not available
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
  uint16_t rpmRaw = (uint16_t)(rpm * 4U); // 0.25 rpm/bit

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
  // 26 bytes fast-packet
  uint8_t p[26];
  memset(p, 0xFF, sizeof(p));
  p[0] = engineInstance; // Engine Instance

  // Oil temperature (0.1K/bit) – usamos el mismo valor si no hay sensor de aceite
  uint16_t t_dK = encodeTemp_dK(tempCelsius);
  p[3] = t_dK & 0xFF;
  p[4] = (t_dK >> 8) & 0xFF;

  // Coolant temperature (0.1K/bit)
  p[5] = t_dK & 0xFF;
  p[6] = (t_dK >> 8) & 0xFF;

  // Alternator potential (0.01 V/bit)
  if (battV > 0.0f && battV < 100.0f) {
    uint16_t alt_dV = (uint16_t)lroundf(battV * 100.0f);
    p[7] = alt_dV & 0xFF;
    p[8] = (alt_dV >> 8) & 0xFF;
  }

  const uint32_t PGN = 127489UL;
  uint32_t extId = ((uint32_t)N2K_PRIO_DYN << 26) | (PGN << 8) | N2K_SA;

  uint8_t f0[8], f1[8], f2[8], f3[8];

  // Fast-packet headers: (seq << 5) | frame#
  f0[0] = (uint8_t)((fpSeq127489 << 5) | 0);
  f0[1] = 26;                        // total bytes
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

// ============ ADC helpers =============
static inline float adcReadAveragedVolts(int pin, int nsamp = ADC_NSAMPLES) {
  uint32_t sum = 0;
  for (int i = 0; i < nsamp; i++) {
    sum += analogRead(pin);
    delayMicroseconds(800);
  }
  float adc = (float)sum / (float)nsamp;
  return (adc / 4095.0f) * ADC_VREF;
}

static inline float readTempC_fromADC(float vNode, float rFixedOhms) {
  if (USE_NTC) {
    // Rntc = Rfixed * V / (Vref - V)  (R fija a 3.3V, NTC a GND)
    vNode = fmaxf(0.001f, fminf(vNode, ADC_VREF - 0.001f)); // evitar 0/∞
    float rNTC = rFixedOhms * vNode / (ADC_VREF - vNode);
    // Ley Beta
    float invT = (1.0f / NTC_T0K) + (1.0f / NTC_BETA) * logf(rNTC / NTC_R0);
    float TK = 1.0f / invT;
    return TK - 273.15f;
  } else {
    // Sensor lineal por interpolación (tu método original)
    return TEMP1 + (V1 - vNode) * (TEMP2 - TEMP1) / (V1 - V2);
  }
}

// ============ Signal K (UDP) ==========
void sendDeltaUDP_Coolant(float tempKelvin) {
  char buf[256];
  int n = snprintf(buf, sizeof(buf),
    "{\"context\":\"vessels.self\",\"updates\":[{\"source\":{\"label\":\"esp32-ntc\"},"
    "\"values\":[{\"path\":\"%s\",\"value\":%.2f}]}]}",
    SK_PATH_COOLANT, tempKelvin
  );
  if (n > 0) {
    skUDP.beginPacket(SK_UDP_HOST, SK_UDP_PORT);
    skUDP.write((const uint8_t*)buf, (size_t)n);
    skUDP.endPacket();
  }
}

// =============== Web server =======================
void handleRoot() {
  String page = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<title>Panel Motor</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'/>"
    "<script src='https://cdn.jsdelivr.net/npm/canvas-gauges@2.1.7/gauge.min.js'></script>"
    "<style>html,body{height:100%;} body{background:#111;margin:0;height:100vh;display:flex;flex-direction:column;justify-content:flex-start;align-items:center;}"
    ".title{color:#fff;font-size:2em;text-align:center;margin-top:1em;}"
    ".gauges{display:flex;flex-wrap:wrap;justify-content:center;align-items:center;gap:3em;margin-top:3em;}"
    ".gauge{background:#222;padding:1em;border-radius:1em;display:flex;flex-direction:column;align-items:center;}"
    ".gauge canvas{display:block;width:150px;height:150px;}"
    ".gauge-value{color:#fff;font-size:1.1em;margin-top:0.4em;text-align:center;}"
    ".btn-parada, .btn-navlight{position:fixed;left:50%;transform:translateX(-50%);z-index:100;}"
    ".btn-parada{bottom:1cm;padding:1em 2em;font-size:1.5em;background:#c00;color:#fff;border:none;border-radius:1em;cursor:pointer;}"
    ".btn-parada.stop{background:#c00;} .btn-parada.run{background:#090;}"
    ".btn-navlight{bottom:6.5em;padding:1em 2em;font-size:1.3em;background:#009;color:#fff;border:none;border-radius:1em;cursor:pointer;}"
    ".btn-navlight.on{background:#090;} .btn-navlight.off{background:#009;}"
    "#alarm-temp{color:#fff; background:#c00; font-size:2em; padding:1em; border-radius:1em; position:fixed; top:2em; left:50%; transform:translateX(-50%); z-index:9999; text-align:center; box-shadow:0 0 20px #c00;}"
    ".calib-toggle-btn{margin-top:2em; font-size:1.2em; background:#090; color:#fff; border:none; border-radius:0.7em; padding:0.7em 2em; cursor:pointer; z-index:10001;}"
    ".calib-pot{color:#fff; background:#222; padding:1em; border-radius:0.7em; box-shadow:0 0 15px #000; font-size:1.2em; text-align:center; position:fixed; left:50%; top:15%; transform:translateX(-50%); z-index:10000; width:320px;}"
    ".calib-pot input{font-size:1em; width:5em;}"
    ".calib-pot button{font-size:1em; margin-left:1em; background:#090; color:#fff; border:none; border-radius:0.4em; padding:0.2em 1em; cursor:pointer;}"
    "</style></head><body>";
  page += "<div class='title'>MOTOR</div>"
    "<div class='gauges'>"
    "<div class='gauge'><canvas id='gauge-temp' width='150' height='150'></canvas><div class='gauge-value' id='val-temp'>---</div><div style='text-align:center;color:#fff;'>Temperatura (&deg;C)</div></div>"
    "<div class='gauge'><canvas id='gauge-rpm' width='150' height='150'></canvas><div class='gauge-value' id='val-rpm'>---</div><div style='text-align:center;color:#fff;'>RPM</div></div>"
    "<div class='gauge'><canvas id='gauge-bat' width='150' height='150'></canvas><div class='gauge-value' id='val-bat'>---</div><div style='text-align:center;color:#fff;'>Batería (V)</div></div>"
    "</div>"
    "<button id='show-pot-calib' class='calib-toggle-btn'>Calibrar potenciómetro</button>"
    "<div class='calib-pot' id='calib-pot-block' style='display:none;'>"
    "Valor del potenciómetro (ohmios): <input id='potval' type='number' min='1000' max='20000' step='100' value='" + String(potValue,0) + "'>"
    "<button onclick='setPot()'>Guardar</button>"
    "<span id='potstatus'></span><br>"
    "<button onclick='hidePotCalib()' style=\"margin-top:1em;background:#444;color:#fff;\">Cerrar ajuste</button>"
    "</div>"
    "<button id='btn-navlight' class='btn-navlight off'>LUZ NAVEGACIÓN OFF</button>"
    "<button id='btn-parada' class='btn-parada stop'>PARADA EMERGENCIA</button>";

  // JS
  page += R"===(
<script>
let gaugeTemp, gaugeRPM, gaugeBat, parado=false, navlight=false;
let potInput = null;

function setPot(){
  let v = parseInt(document.getElementById('potval').value);
  fetch('/potval?valor='+v).then(r=>r.json()).then(j=>{
    document.getElementById('potstatus').textContent = 'Potenciómetro = ' + j.pot + ' Ω';
  });
}
function hidePotCalib(){ document.getElementById('calib-pot-block').style.display = 'none'; }
function updateBtn(){ const btn=document.getElementById('btn-parada'); if(parado){btn.textContent='REARMAR MOTOR';btn.className='btn-parada run';}else{btn.textContent='PARADA EMERGENCIA';btn.className='btn-parada stop';}}
function updateNavBtn(){ const btn=document.getElementById('btn-navlight'); if(navlight){btn.textContent='LUZ NAVEGACIÓN ON';btn.className='btn-navlight on';}else{btn.textContent='LUZ NAVEGACIÓN OFF';btn.className='btn-navlight off';}}

document.addEventListener('DOMContentLoaded',function(){
  gaugeTemp = new RadialGauge({renderTo:'gauge-temp',minValue:0,maxValue:120,units:'°C',majorTicks:[0,20,40,60,80,100,120],minorTicks:4,highlights:[{from:90,to:120,color:'rgba(200,50,50,.75)'}],value:0,width:150,height:150}).draw();
  gaugeRPM  = new RadialGauge({renderTo:'gauge-rpm', minValue:0,maxValue:4000,units:'rpm',majorTicks:[0,500,1000,1500,2000,2500,3000,3500,4000],minorTicks:4,highlights:[{from:3500,to:4000,color:'rgba(200,50,50,.75)'}],value:0,width:150,height:150}).draw();
  gaugeBat  = new RadialGauge({renderTo:'gauge-bat', minValue:10,maxValue:15,units:'V',  majorTicks:[10,11,12,13,14,15],minorTicks:5,highlights:[{from:10,to:11.5,color:'rgba(200,50,50,.75)'},{from:14.5,to:15,color:'rgba(200,200,50,.75)'}],value:0,width:150,height:150}).draw();

  potInput = document.getElementById('potval');
  document.getElementById('show-pot-calib').onclick = function(){ document.getElementById('calib-pot-block').style.display = 'block'; };

  setInterval(()=>{
    fetch('/vals').then(r=>r.json()).then(j=>{
      gaugeTemp.value = j.temp; document.getElementById('val-temp').textContent = j.temp.toFixed(1);
      gaugeRPM.value  = j.rpm;  document.getElementById('val-rpm').textContent  = j.rpm;
      gaugeBat.value  = j.bat;  document.getElementById('val-bat').textContent  = j.bat.toFixed(2);
      parado = j.parado; updateBtn(); navlight = j.navlight; updateNavBtn();
      if (document.activeElement !== potInput) { potInput.value = j.pot; }
      document.getElementById('potstatus').textContent = 'Potenciómetro = ' + j.pot + ' Ω';
      if (j.temp > 110) { if (!document.getElementById('alarm-temp')) { let alarm = document.createElement('div'); alarm.id = 'alarm-temp'; alarm.innerHTML = '&#9888; <b>PELIGRO:</b> Temp motor muy alta (' + j.temp.toFixed(1) + ' °C)'; document.body.appendChild(alarm);} } else { let alarm = document.getElementById('alarm-temp'); if (alarm) alarm.remove(); }
    });
  }, 500);

  document.getElementById('btn-parada').onclick=function(){ parado = !parado; updateBtn(); fetch('/parada',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({parar:parado})}).then(()=>{}); };
  document.getElementById('btn-navlight').onclick=function(){ navlight = !navlight; updateNavBtn(); fetch('/navlight',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({on:navlight})}).then(()=>{}); };
});
</script>
)===";
  page += "</body></html>";
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
      potValue = v;  // R fija del divisor NTC
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

// ================== Setup / Loop ====================
void setup() {
  Serial.begin(115200);

  // WiFi
  WiFi.config(local_IP, gateway, subnet, dns);
  WiFi.begin(ssid, password);
  Serial.println("Conectando a WiFi...");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println(); Serial.print("WiFi conectado. IP: "); Serial.println(WiFi.localIP());

  // SPI + CAN
  SPI.begin(18, 19, 23, PIN_CS);
  if (CAN.begin(MCP_ANY, CAN_250KBPS, MCP_8MHZ) != CAN_OK) {
    Serial.println("Error inicializando MCP_CAN");
    while (1) delay(100);
  }
  CAN.setMode(MCP_NORMAL);

  // GPIO
  pinMode(PIN_INT, INPUT);
  pinMode(PIN_ADC, INPUT);
  pinMode(PIN_BATTERY, INPUT);
  pinMode(PIN_PARADA, OUTPUT);   digitalWrite(PIN_PARADA, LOW);
  pinMode(PIN_NAVLIGHT, OUTPUT); digitalWrite(PIN_NAVLIGHT, LOW);

  // ADC atenuación para ~0..3.3V
  analogSetPinAttenuation(PIN_ADC, ADC_11db);
  analogSetPinAttenuation(PIN_BATTERY, ADC_11db);

  // Prefs
  prefs.begin("calib", false);
  potValue = prefs.getFloat("potValue", 3000.0f);
  prefs.end();

  // Web
  server.on("/", handleRoot);
  server.on("/vals", handleVals);
  server.on("/parada", HTTP_POST, handleParada);
  server.on("/navlight", HTTP_POST, handleNavLight);
  server.on("/potval", handlePotVal);
  server.begin();
  Serial.println("Servidor web iniciado.");

  // N2K
  sendPGN60928_AddressClaim();

  // Signal K UDP
  skUDP.begin(0); // puerto efímero de salida
}

void loop() {
  // --- Temperatura ---
  float vNode = adcReadAveragedVolts(PIN_ADC, ADC_NSAMPLES);
  float tRaw = readTempC_fromADC(vNode, potValue);

  // Calibración lineal (post-proceso)
  if (ENABLE_TCAL) {
    tempC = T_CAL_A * tRaw + T_CAL_B;
  } else {
    tempC = tRaw;
  }
  if (ENABLE_TCLAMP) {
    if (tempC < T_MIN_C) tempC = T_MIN_C;
    if (tempC > T_MAX_C) tempC = T_MAX_C;
  }

  // --- Batería (con media) ---
  float vBatNode = adcReadAveragedVolts(PIN_BATTERY, ADC_NSAMPLES);
  batteryV = vBatNode * VBAT_GAIN;

  // --- N2K TX: RPM (500 ms) ---
  static unsigned long lastTxRPM = 0;
  if (millis() - lastTxRPM >= 500) {
    lastTxRPM = millis();
    sendPGN127488(0, rpmSim);
  }

  // --- N2K TX: 130316 Temperature, Extended Range (1 s) ---
  static unsigned long lastTx316 = 0;
  if (millis() - lastTx316 >= 1000) {
    lastTx316 = millis();
    byte data_130316[8] = {0};
    data_130316[0] = 0x00; // SID
    data_130316[1] = 0x00; // Instance
    data_130316[2] = 0x16; // Source (ajusta si quieres)

    // 24 bits, milliKelvin, little-endian
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

  // --- N2K TX: 127489 Engine Params, Dynamic (1 s) ---
  static unsigned long lastTx489 = 0;
  if (millis() - lastTx489 >= 1000) {
    lastTx489 = millis();
    sendPGN127489(0, tempC, batteryV);
  }

  // --- Signal K: Delta UDP coolantTemperature (1 s) ---
  static unsigned long lastSk = 0;
  if (millis() - lastSk >= 1000) {
    lastSk = millis();
    float Tk = tempC + 273.15f;
    sendDeltaUDP_Coolant(Tk);
  }

  server.handleClient();
}
