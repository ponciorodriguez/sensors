#include <SPI.h>
#include <mcp_can.h>
#include <WiFi.h>
#include <WebServer.h>

#define PIN_CS     5
#define PIN_INT    4
#define PIN_ADC    34
#define PIN_PARADA 27
#define PIN_NAVLIGHT 26
#define PIN_BATTERY 35  // Pin para leer nivel de batería

const char* ssid = "calaestancia";
const char* password = "Catieta1";

IPAddress local_IP(192, 168, 1, 63);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(192, 168, 1, 1);

MCP_CAN CAN(PIN_CS);
WebServer server(80);

float tempC = 0.0;
float batteryV = 0.0;
const int rpmSim = 2500;
volatile bool motorParado = false;
volatile bool navLightOn = false;

void handleRoot() {
  String page = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  page += "<title>Panel Motor</title>";
  page += "<script src='https://cdn.jsdelivr.net/npm/canvas-gauges@2.1.7/gauge.min.js'></script>";
  page += "<style>";
  page += "html,body{height:100%;} body{background:#111;margin:0;height:100vh;display:flex;flex-direction:column;justify-content:flex-start;align-items:center;}";
  page += ".title{color:#fff;font-size:2em;text-align:center;margin-top:1em;}";
  page += ".gauges{display:flex;justify-content:center;align-items:center;gap:3em;margin-top:3em;}";
  page += ".gauge{background:#222;padding:1em;border-radius:1em;display:flex;flex-direction:column;align-items:center;}";
  page += ".gauge canvas{display:block;width:150px;height:150px;}";
  page += ".gauge-value{color:#fff;font-size:1.1em;margin-top:0.4em;text-align:center;}";
  page += ".btn-parada{position:fixed;left:50%;transform:translateX(-50%);bottom:1cm;padding:1em 2em;font-size:1.5em;background:#c00;color:#fff;border:none;border-radius:1em;cursor:pointer;z-index:99;}";
  page += ".btn-parada.stop{background:#c00;} .btn-parada.run{background:#090;}";
  page += ".btn-navlight{position:fixed;left:50%;transform:translateX(-50%);bottom:6.5em;padding:1em 2em;font-size:1.3em;background:#009;color:#fff;border:none;border-radius:1em;cursor:pointer;z-index:99;}";
  page += ".btn-navlight.on{background:#090;} .btn-navlight.off{background:#009;}";
  page += "#alarm-temp{color:#fff; background:#c00; font-size:2em; padding:1em; border-radius:1em; position:fixed; top:2em; left:50%; transform:translateX(-50%); z-index:9999; text-align:center; box-shadow:0 0 20px #c00;}";
  page += "</style>";
  page += "</head><body>";
  page += "<div class='title'>MOTOR</div>";
  page += "<div class='gauges'>";
  page += "<div class='gauge'>";
  page += "<canvas id='gauge-temp' width='150' height='150'></canvas>";
  page += "<div class='gauge-value' id='val-temp'>---</div>";
  page += "<div style='text-align:center;color:#fff;'>Temperatura (&deg;C)</div></div>";
  page += "<div class='gauge'>";
  page += "<canvas id='gauge-rpm' width='150' height='150'></canvas>";
  page += "<div class='gauge-value' id='val-rpm'>---</div>";
  page += "<div style='text-align:center;color:#fff;'>RPM</div></div>";
  page += "<div class='gauge'>";
  page += "<canvas id='gauge-bat' width='150' height='150'></canvas>";
  page += "<div class='gauge-value' id='val-bat'>---</div>";
  page += "<div style='text-align:center;color:#fff;'>Batería (V)</div></div>";
  page += "</div>";
  page += "<button id='btn-navlight' class='btn-navlight off'>LUZ NAVEGACIÓN OFF</button>";
  page += "<button id='btn-parada' class='btn-parada stop'>PARADA EMERGENCIA</button>";
  page += "<script>";
  page += "let gaugeTemp, gaugeRPM, gaugeBat, parado=false, navlight=false;";
  page += "function updateBtn(){";
  page += " const btn=document.getElementById('btn-parada');";
  page += " if(parado){btn.textContent='REARMAR MOTOR';btn.className='btn-parada run';}else{btn.textContent='PARADA EMERGENCIA';btn.className='btn-parada stop';}";
  page += "}";
  page += "function updateNavBtn(){";
  page += " const btn=document.getElementById('btn-navlight');";
  page += " if(navlight){btn.textContent='LUZ NAVEGACIÓN ON';btn.className='btn-navlight on';}else{btn.textContent='LUZ NAVEGACIÓN OFF';btn.className='btn-navlight off';}";
  page += "}";
  page += "document.addEventListener('DOMContentLoaded',function(){";
  page += "gaugeTemp = new RadialGauge({renderTo:'gauge-temp',minValue:0,maxValue:120,units:'°C',majorTicks:[0,20,40,60,80,100,120],minorTicks:4,highlights:[{from:90,to:120,color:'rgba(200,50,50,.75)'}],value:0,width:150,height:150}).draw();";
  page += "gaugeRPM = new RadialGauge({renderTo:'gauge-rpm',minValue:0,maxValue:4000,units:'rpm',majorTicks:[0,500,1000,1500,2000,2500,3000,3500,4000],minorTicks:4,highlights:[{from:3500,to:4000,color:'rgba(200,50,50,.75)'}],value:0,width:150,height:150}).draw();";
  page += "gaugeBat = new RadialGauge({renderTo:'gauge-bat',minValue:10,maxValue:15,units:'V',majorTicks:[10,11,12,13,14,15],minorTicks:5,highlights:[{from:10,to:11.5,color:'rgba(200,50,50,.75)'},{from:14.5,to:15,color:'rgba(200,200,50,.75)'}],value:0,width:150,height:150}).draw();";
  page += "setInterval(()=>{fetch('/vals').then(r=>r.json()).then(j=>{";
  page += "gaugeTemp.value = j.temp;";
  page += "document.getElementById('val-temp').textContent = j.temp.toFixed(1);";
  page += "gaugeRPM.value = j.rpm;";
  page += "document.getElementById('val-rpm').textContent = j.rpm;";
  page += "gaugeBat.value = j.bat;";
  page += "document.getElementById('val-bat').textContent = j.bat.toFixed(2);";
  page += "parado = j.parado; updateBtn();";
  page += "navlight = j.navlight; updateNavBtn();";
  page += "if (j.temp > 110) {";
  page += "  if (!document.getElementById('alarm-temp')) {";
  page += "    let alarm = document.createElement('div');";
  page += "    alarm.id = 'alarm-temp';";
  page += "    alarm.innerHTML = '&#9888; <b>PELIGRO:</b> Temperatura de motor demasiado alta (' + j.temp.toFixed(1) + ' °C)';";
  page += "    document.body.appendChild(alarm);";
  page += "    /* Opcional: sonido de alarma";
  page += "    let audio = new Audio('/alarma.mp3'); audio.play(); */";
  page += "  }";
  page += "} else {";
  page += "  let alarm = document.getElementById('alarm-temp');";
  page += "  if (alarm) alarm.remove();";
  page += "}";
  page += "});},500);";
  page += "document.getElementById('btn-parada').onclick=function(){";
  page += " fetch('/parada',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({parar:!parado})}).then(()=>{parado=!parado;updateBtn();});";
  page += "};";
  page += "document.getElementById('btn-navlight').onclick=function(){";
  page += " fetch('/navlight',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({on:!navlight})}).then(()=>{navlight=!navlight;updateNavBtn();});";
  page += "};";
  page += "});";
  page += "</script>";
  page += "</body></html>";
  server.send(200, "text/html", page);
}

void handleVals() {
  String json = "{";
  json += "\"temp\":" + String(tempC, 1) + ",";
  json += "\"rpm\":" + String(rpmSim) + ",";
  json += "\"bat\":" + String(batteryV, 2) + ",";
  json += "\"parado\":" + String(motorParado ? "true" : "false") + ",";
  json += "\"navlight\":" + String(navLightOn ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleParada() {
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    bool parar = body.indexOf("true") > 0 || body.indexOf("parar\":true") > 0;
    motorParado = parar;
    digitalWrite(PIN_PARADA, motorParado ? HIGH : LOW);
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(400, "application/json", "{\"ok\":false}");
  }
}

void handleNavLight() {
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    bool turnOn = body.indexOf("true") > 0 || body.indexOf("on\":true") > 0;
    navLightOn = turnOn;
    digitalWrite(PIN_NAVLIGHT, navLightOn ? HIGH : LOW);
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(400, "application/json", "{\"ok\":false}");
  }
}

void setup() {
  Serial.begin(115200);

  WiFi.config(local_IP, gateway, subnet, dns);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  SPI.begin(18, 19, 23, PIN_CS);
  if (CAN.begin(MCP_ANY, CAN_250KBPS, MCP_8MHZ) != CAN_OK) {
    while (1);
  }
  CAN.setMode(MCP_NORMAL);

  pinMode(PIN_INT, INPUT);
  pinMode(PIN_ADC, INPUT);
  pinMode(PIN_BATTERY, INPUT);
  pinMode(PIN_PARADA, OUTPUT);
  digitalWrite(PIN_PARADA, LOW);
  pinMode(PIN_NAVLIGHT, OUTPUT);
  digitalWrite(PIN_NAVLIGHT, LOW);

  server.on("/", handleRoot);
  server.on("/vals", handleVals);
  server.on("/parada", HTTP_POST, handleParada);
  server.on("/navlight", HTTP_POST, handleNavLight);
  server.begin();
}

void loop() {
  int adcRaw = analogRead(PIN_ADC);
  tempC = (adcRaw / 4095.0f) * 120.0f;

  int adcBat = analogRead(PIN_BATTERY);
  batteryV = adcBat * (16.0 / 4095.0); // Ajusta el 16.0 al máximo de tu batería real

  // --- ENVÍA RPM (PGN 127488) ---
  static unsigned long lastTx = 0;
  if (millis() - lastTx >= 500) {
    lastTx = millis();

    byte data_rpm[8] = {0};
    data_rpm[0] = 0; // Engine Instance
    uint16_t rpmRaw = rpmSim * 4;
    data_rpm[1] = rpmRaw & 0xFF;
    data_rpm[2] = (rpmRaw >> 8) & 0xFF;
    data_rpm[3] = 0xFF;
    data_rpm[4] = 0xFF;
    data_rpm[5] = 0xFF;
    data_rpm[6] = 0xFF;
    data_rpm[7] = 0xFF;
    unsigned long pgn_rpm = 127488;
    unsigned long extId_rpm = (6UL << 26) | (pgn_rpm << 8) | 0;
    CAN.sendMsgBuf(extId_rpm, 1, 8, data_rpm);
  }



  // --- ENVÍA TEMPERATURA AGUA AMBIENTE NMEA2000 (PGN 130310) ---
  static unsigned long lastEnvTx = 0;
  if (millis() - lastEnvTx >= 1000) {
    lastEnvTx = millis();

    byte data_env[8] = {0};
    data_env[0] = 0; // SID
    data_env[1] = 0; // Water temperature instance (0 = agua)
    uint16_t tempK100 = (uint16_t)((tempC + 273.15) * 1.0);
    data_env[2] = tempK100 & 0xFF;        // Temp LSB
    data_env[3] = (tempK100 >> 8) & 0xFF; // Temp MSB
    data_env[4] = 0xFF; // Humedad relativa no disponible
    data_env[5] = 0xFF; // Humedad relativa no disponible
    data_env[6] = 0xFF; // Temp dewpoint no disponible
    data_env[7] = 0xFF; // Temp dewpoint no disponible

    unsigned long pgn_env = 130310;
    unsigned long extId_env = (6UL << 26) | (pgn_env << 8) | 0;
    CAN.sendMsgBuf(extId_env, 1, 8, data_env);
  }

  server.handleClient();
}