/**
 * =============================================================================
 * SISTEMA DE MONITOREO DE CALIDAD DEL AIRE - ESP32
 * =============================================================================
 * Descripción:
 *   Sistema embebido de monitoreo ambiental en tiempo real que mide material
 *   particulado (PM2.5, PM10), gases tóxicos, temperatura, humedad y presión
 *   atmosférica. Opera con un hilo FreeRTOS dedicado a medición/periféricos;
 *   el servidor web asíncrono es atendido por la tarea async_tcp en núcleo 1.
 *
 * Hardware:
 *   - Microcontrolador : ESP32 DevKit
 *   - Sensor PM        : PMS5003        (UART2 — RX:GPIO16, TX:GPIO17)
 *   - Sensor presión/T : BMP280         (I2C   — SDA:GPIO21, SCL:GPIO22, Dir:0x76)
 *   - Sensor hum/T     : DHT11          (Digital — GPIO27)
 *   - Sensor gases     : MQ-135         (ADC   — GPIO34, con divisor de voltaje)
 *   - Pantalla         : LCD 16x2 I2C   (I2C   — Dir:0x27)
 *   - Actuador         : LED/Buzzer     (Digital — GPIO13)
 *
 * Arquitectura de hilos (FreeRTOS):
 *   - measurementTask (núcleo 0): Lectura de sensores, corrección Barkjohn,
 *     promedio móvil, lógica de alerta, control de LCD/buzzer/LED.
 *     Al finalizar cada ciclo actualiza datos compartidos e historyBuf.
 *   - async_tcp (núcleo 1): Tarea interna de AsyncTCP/ESPAsyncWebServer.
 *     Atiende todas las conexiones HTTP: login, dashboard, polling, histórico
 *     y desactivación de alarma.
 *   - Mutex 'dataMutex': Protege DatosAmbientales e historyBuf entre hilos.
 *
 * Módulo servidor web:
 *   - Librería    : ESPAsyncWebServer + AsyncTCP
 *   - Auth        : Formulario HTML + token de sesión (esp_random), expira 30 min,
 *                   bloqueo por cuenta tras MAX_LOGIN_ATTEMPTS intentos fallidos.
 *   - Polling     : GET /data    → JSON con lectura actual (cada 2 s desde JS)
 *   - Histórico   : GET /history → JSON con últimas HISTORY_SIZE lecturas
 *                   (campos: labels, pm25, temp, gas)
 *   - Alarma      : POST /alarm/off — desactiva alarma física
 *   - Endpoints   : GET /       → redirige a /login o sirve dashboard
 *                   GET /login  → formulario de acceso
 *                   POST /login → valida credenciales, emite cookie de sesión
 *
 * Corrección PM2.5 (Barkjohn et al., 2021 — EPA):
 *   PM2.5_corr = 0.541 × PM2.5_raw − 0.0618 × HR + 0.00534 × T + 3.634
 *
 * Conversión MQ-135 a ppm (Gironi, 2014):
 *   Rs_corr = Rs / (−0.00268×T + 0.01383×HR + 1.2698)
 *   ppm     = 102.2 × (Rs_corr / Ro)^(−2.473)
 *
 * Nota: para más información consultar la Wiki del proyecto
 * =============================================================================
 */

#define ASYNCWEBSERVER_REGEX 0

// ─── Librerías ────────────────────────────────────────────────────────────────
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include "PMS.h"
#include "DHT.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>

// ─── LCD ──────────────────────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ─── Wi-Fi ────────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "FAMILIA_SEPULVEDA";
const char* WIFI_PASSWORD = "MarcoAurelio48";

// ─── MQTT ─────────────────────────────────────────────────────────────────────
#define MQTT_HOST    IPAddress(192, 168, 20, 27)
#define MQTT_PORT    1883
#define MQTT_USER    "esp32_01"
#define MQTT_PASS    "mosquitto-esp32-01-2234" 
#define MQTT_TOPIC   "dispositivos/esp32_01/datos"
#define MQTT_CLIENT  "esp32_01"
#define MQTT_TOPIC_CMD  "dispositivos/esp32_01/comandos"

AsyncMqttClient mqttClient;

// ─── Autenticación ────────────────────────────────────────────────────────────
struct UserAccount {
  const char* user;
  const char* pass;
  int         loginAttempts;
  uint32_t    lockoutUntil;
};

UserAccount accounts[] = {
  { "admin",    "sabana2026", 0, 0 },
  { "alcaldia", "monitoreo1", 0, 0 },
  { "tecnico",  "aire2026",   0, 0 },
};
const int NUM_ACCOUNTS = sizeof(accounts) / sizeof(accounts[0]);

const int      MAX_SESSIONS       = 8;
const uint32_t SESSION_MS         = 30UL * 60UL * 1000UL;
const int      MAX_LOGIN_ATTEMPTS = 5;
const uint32_t LOCKOUT_MS         = 5UL  * 60UL * 1000UL;

struct Session {
  char     token[17];
  uint32_t expiry;
  bool     active;
};
Session sessions[MAX_SESSIONS] = {};

// ─── Hardware ─────────────────────────────────────────────────────────────────
#define DHTPIN     27
#define DHTTYPE    DHT11
#define MQ135_PIN  34
#define ALARMA_PIN 13
#define VENTANA    10

// ─── Calibración MQ-135 ───────────────────────────────────────────────────────
float       Ro     = 10000.0;
const float RL     = 10000.0;
const float PARA_A = 102.2;
const float PARA_B = -2.473;

// ─── Umbrales de Alerta ───────────────────────────────────────────────────────
#define PM25_UMBRAL  35.5
#define GAS_UMBRAL   200.0
#define TEMP_MIN     17.0
#define TEMP_MAX     27.0
#define HUM_MIN      30.0
#define HUM_MAX      70.0
#define PRES_MIN    740.0
#define PRES_MAX    754.0

// ─── Instancias de sensores ───────────────────────────────────────────────────
DHT             dht(DHTPIN, DHTTYPE);
Adafruit_BMP280 bmp;
PMS             pms(Serial2);
PMS::DATA       pmsData;

// ─── Datos compartidos entre hilos ───────────────────────────────────────────
SemaphoreHandle_t dataMutex;

struct DatosAmbientales {
  float temp, hum, pres, pm25, pm10, gasPPM;
  bool  alarmaActiva, datosListos;
  bool  alertaPM25, alertaGas, alertaTemp, alertaHum, alertaPres;
  bool  alarmaSilenciada;
};
DatosAmbientales datos = {};

// ─── Histórico de lecturas (protegido por dataMutex) ─────────────────────────
struct HistoryEntry {
  float pm25, pm10, gasPPM, temp, hum, pres;
};
const int    HISTORY_SIZE = 50;
HistoryEntry historyBuf[HISTORY_SIZE];
int          historyIdx   = 0;
int          historyCount = 0;

// ─── Promedio móvil (solo measurementTask) ────────────────────────────────────
float pmBuffer[VENTANA]  = {};
float gasBuffer[VENTANA] = {};
int   mvCount = 0, mvIdx = 0;

// ─── FreeRTOS task handle ─────────────────────────────────────────────────────
TaskHandle_t measurementTaskHandle;

// ─── Cola MQTT (measurementTask → mqttTask) ───────────────────────────────────
QueueHandle_t mqttQueue;
TaskHandle_t  mqttTaskHandle;

// ─── Web Server ───────────────────────────────────────────────────────────────
AsyncWebServer server(80);

// ─── HTML Login (PROGMEM) — Tema claro ───────────────────────────────────────
const char LOGIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="es"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Acceso — Monitoreo Calidad del Aire</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;600&family=IBM+Plex+Sans:wght@300;400;500&display=swap');
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'IBM Plex Sans',sans-serif;background:#eef2f7;display:flex;align-items:center;justify-content:center;min-height:100vh}
.wrap{width:360px}
.logo{display:flex;align-items:center;gap:12px;margin-bottom:24px}
.logo-icon{width:40px;height:40px;background:#276749;border-radius:10px;display:flex;align-items:center;justify-content:center;box-shadow:0 2px 8px rgba(39,103,73,.3)}
.logo-icon svg{width:22px;height:22px;fill:#fff}
.logo-title{font-family:'IBM Plex Mono',monospace;font-size:.82rem;color:#276749;letter-spacing:.05em;font-weight:600}
.logo-sub{font-size:.7rem;color:#718096;margin-top:2px}
.card{background:#fff;border:1px solid #e2e8f0;border-radius:14px;padding:30px;box-shadow:0 4px 20px rgba(0,0,0,.07)}
h1{font-size:1.05rem;font-weight:600;color:#1a202c;margin-bottom:4px}
.sub{font-size:.78rem;color:#718096;margin-bottom:22px}
label{display:block;font-size:.75rem;color:#4a5568;margin-bottom:5px;font-family:'IBM Plex Mono',monospace;font-weight:600;letter-spacing:.04em}
input{width:100%;padding:.65rem .85rem;background:#f7fafc;border:1.5px solid #e2e8f0;border-radius:8px;font-size:.9rem;color:#1a202c;margin-bottom:14px;font-family:'IBM Plex Sans',sans-serif;transition:border-color .2s,box-shadow .2s}
input:focus{outline:none;border-color:#276749;box-shadow:0 0 0 3px rgba(39,103,73,.1);background:#fff}
button{width:100%;padding:.75rem;background:#276749;color:#fff;border:none;border-radius:8px;font-size:.92rem;cursor:pointer;font-family:'IBM Plex Sans',sans-serif;font-weight:500;letter-spacing:.02em;transition:background .2s,transform .1s;box-shadow:0 2px 6px rgba(39,103,73,.3)}
button:hover{background:#22543d}
button:active{transform:scale(.98)}
.err{background:#fff5f5;border:1.5px solid #fc8181;color:#c53030;padding:.65rem .85rem;border-radius:8px;font-size:.78rem;margin-bottom:14px;display:none;font-family:'IBM Plex Mono',monospace}
.err.show{display:block}
.foot{text-align:center;font-size:.7rem;color:#a0aec0;margin-top:18px}
</style></head><body>
<div class="wrap">
  <div class="logo">
    <div class="logo-icon"><svg viewBox="0 0 24 24"><path d="M12 2a10 10 0 1 0 10 10A10 10 0 0 0 12 2zm1 15h-2v-6h2zm0-8h-2V7h2z"/></svg></div>
    <div><div class="logo-title">AIRWATCH / SABANA</div><div class="logo-sub">Universidad de La Sabana · IoT 2026-1</div></div>
  </div>
  <div class="card">
    <h1>Acceso al tablero</h1>
    <p class="sub">Solo personal autorizado de la alcaldía</p>
    <div class="err %ERR_CLASS%">%ERR_MSG%</div>
    <form method="POST" action="/login">
      <label>USUARIO</label>
      <input type="text" name="user" autocomplete="username" required>
      <label>CONTRASEÑA</label>
      <input type="password" name="pass" autocomplete="current-password" required>
      <button type="submit">Ingresar al sistema</button>
    </form>
  </div>
  <div class="foot">Acceso restringido · Red WLAN Alcaldía</div>
</div>
</body></html>
)rawliteral";

// ─── HTML Dashboard (PROGMEM) — Tema claro ────────────────────────────────────
const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="es"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tablero — Calidad del Aire</title>
<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.1/chart.umd.min.js"></script>
<style>
@import url('https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;600&family=IBM+Plex+Sans:wght@300;400;500&display=swap');
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'IBM Plex Sans',sans-serif;background:#eef2f7;color:#1a202c;padding:12px;min-height:100vh}

/* Header */
.hdr{display:flex;justify-content:space-between;align-items:center;padding:13px 18px;border-radius:12px;margin-bottom:12px;border:1.5px solid transparent;transition:background .5s,border-color .5s;box-shadow:0 2px 8px rgba(0,0,0,.06)}
.hdr.ok{background:#f0fff4;border-color:#9ae6b4}
.hdr.al{background:#fff5f5;border-color:#fc8181;animation:flicker 2s infinite}
@keyframes flicker{0%,100%{border-color:#fc8181}50%{border-color:#c53030}}
.hdr-left h1{font-size:.75rem;color:#718096;font-weight:400;margin-bottom:2px}
.hdr-left h2{font-size:1.15rem;font-weight:600;font-family:'IBM Plex Mono',monospace}
.hdr.ok h2{color:#276749}
.hdr.al h2{color:#c53030}
.hdr-right{text-align:right;flex-shrink:0}
.live-dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:5px;transition:background .3s}
.live-dot.online{background:#38a169;animation:pulse 2s infinite}
.live-dot.offline{background:#e53e3e}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.25}}
.live-label{font-size:.7rem;color:#718096;font-family:'IBM Plex Mono',monospace}
.ts{font-size:.65rem;color:#a0aec0;margin-top:3px;font-family:'IBM Plex Mono',monospace}

/* Sensor grid */
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(145px,1fr));gap:8px;margin-bottom:12px}
.card{background:#fff;border:1.5px solid #e2e8f0;border-radius:10px;padding:13px;transition:border-color .3s,background .3s,box-shadow .3s;box-shadow:0 1px 4px rgba(0,0,0,.05)}
.card.al{border-color:#fc8181;background:#fff5f5;box-shadow:0 2px 8px rgba(197,48,48,.1)}
.card .lbl{font-size:.67rem;color:#718096;font-family:'IBM Plex Mono',monospace;letter-spacing:.04em;margin-bottom:7px;font-weight:600}
.card .val{font-size:1.7rem;font-weight:600;font-family:'IBM Plex Mono',monospace;line-height:1;margin-bottom:3px;color:#1a202c}
.card.al .val{color:#c53030}
.card .unit{font-size:.64rem;color:#a0aec0}
.card .warn{font-size:.64rem;color:#c53030;margin-top:5px;font-family:'IBM Plex Mono',monospace;font-weight:600}
.card .thresh{font-size:.6rem;color:#cbd5e0;margin-top:2px}

/* Charts */
.charts-row{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:12px}
@media(max-width:600px){.charts-row{grid-template-columns:1fr}}
.chart-box{background:#fff;border:1.5px solid #e2e8f0;border-radius:10px;padding:14px;box-shadow:0 1px 4px rgba(0,0,0,.05)}
.chart-hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px}
.chart-hdr h3{font-size:.72rem;color:#4a5568;font-family:'IBM Plex Mono',monospace;letter-spacing:.04em;font-weight:600}
.legend{display:flex;gap:12px;flex-wrap:wrap}
.legend span{display:flex;align-items:center;gap:5px;font-size:.65rem;color:#718096}
.legend i{width:14px;height:2px;display:inline-block;border-radius:1px}

/* Alarm button */
.btn-al{display:none;width:100%;padding:13px;background:#fff5f5;border:1.5px solid #fc8181;color:#c53030;border-radius:10px;font-size:.88rem;cursor:pointer;font-family:'IBM Plex Mono',monospace;letter-spacing:.04em;margin-bottom:12px;transition:background .2s,box-shadow .2s;font-weight:600}
.btn-al:hover{background:#fed7d7;box-shadow:0 2px 8px rgba(197,48,48,.15)}
.btn-al:disabled{opacity:.5;cursor:default}

.btn-enable{display:none;width:100%;padding:11px;background:#fffbeb;border:1.5px solid #f6ad55;color:#c05621;border-radius:10px;font-size:.85rem;cursor:pointer;font-family:'IBM Plex Mono',monospace;letter-spacing:.04em;margin-bottom:12px;transition:background .2s;font-weight:600}
.btn-enable:hover{background:#feebc8}

/* Footer */
.foot{text-align:center;font-size:.65rem;color:#cbd5e0;font-family:'IBM Plex Mono',monospace;padding-bottom:8px}
</style></head><body>

<div class="hdr ok" id="hdr">
  <div class="hdr-left">
    <h1>AIRWATCH / SABANA CENTRO — Sistema de Monitoreo IoT</h1>
    <h2 id="estado">⠿ Conectando...</h2>
  </div>
  <div class="hdr-right">
    <div><span class="live-dot online" id="dot"></span><span class="live-label">ACTUALIZACIÓN · 2s</span></div>
    <div class="ts" id="ts">--:--:--</div>
  </div>
</div>

<div class="grid">
  <div class="card" id="c-pm25">
    <div class="lbl">PM2.5 CORREGIDO</div>
    <div class="val" id="v-pm25">--.-</div>
    <div class="unit">µg/m³</div>
    <div class="thresh">umbral · 35.5 µg/m³ IBOCA</div>
    <div class="warn" id="w-pm25"></div>
  </div>
  <div class="card" id="c-pm10">
    <div class="lbl">PM10 (REF.)</div>
    <div class="val" id="v-pm10">---</div>
    <div class="unit">µg/m³ · solo informativo</div>
    <div class="thresh">sin umbral activo</div>
  </div>
  <div class="card" id="c-gas">
    <div class="lbl">GAS</div>
    <div class="val" id="v-gas">---</div>
    <div class="unit">ppm</div>
    <div class="thresh">umbral · 200 ppm NIOSH REL</div>
    <div class="warn" id="w-gas"></div>
  </div>
  <div class="card" id="c-temp">
    <div class="lbl">TEMPERATURA</div>
    <div class="val" id="v-temp">--.-</div>
    <div class="unit">°C</div>
    <div class="thresh">rango · 17–27 °C</div>
    <div class="warn" id="w-temp"></div>
  </div>
  <div class="card" id="c-hum">
    <div class="lbl">HUMEDAD REL.</div>
    <div class="val" id="v-hum">--</div>
    <div class="unit">% HR</div>
    <div class="thresh">rango · 30–70 %</div>
    <div class="warn" id="w-hum"></div>
  </div>
  <div class="card" id="c-pres">
    <div class="lbl">PRESIÓN ATM.</div>
    <div class="val" id="v-pres">---.-</div>
    <div class="unit">hPa</div>
    <div class="thresh">rango · 740–754 hPa</div>
    <div class="warn" id="w-pres"></div>
  </div>
</div>

<!-- Dos gráficas en fila -->
<div class="charts-row">
  <!-- Gráfica 1: PM2.5 + Temperatura -->
  <div class="chart-box">
    <div class="chart-hdr">
      <h3>PM2.5 / TEMPERATURA</h3>
      <div class="legend">
        <span><i style="background:#e53e3e"></i>PM2.5 (µg/m³)</span>
        <span><i style="background:#3182ce;border-top:1px dashed #3182ce;height:0"></i>Temp (°C)</span>
      </div>
    </div>
    <div style="position:relative;height:170px">
      <canvas id="chartPM" role="img" aria-label="Histórico de PM2.5 y Temperatura">Gráfico PM2.5 / Temperatura.</canvas>
    </div>
  </div>
  <!-- Gráfica 2: Gas ppm -->
  <div class="chart-box">
    <div class="chart-hdr">
      <h3>GAS</h3>
      <div class="legend">
        <span><i style="background:#d69e2e"></i>Gas (ppm)</span>
        <span><i style="background:rgba(197,48,48,.4);border-top:1px dashed rgba(197,48,48,.6);height:0"></i>Límite 200 ppm</span>
      </div>
    </div>
    <div style="position:relative;height:170px">
      <canvas id="chartGas" role="img" aria-label="Histórico de concentración de gas">Gráfico Gas ppm.</canvas>
    </div>
  </div>
</div>

<button class="btn-al"     id="btn-al"     onclick="apagar()">▲ DESACTIVAR ALARMA FÍSICA</button>
<button class="btn-enable" id="btn-enable" onclick="habilitar()">↺ RE-HABILITAR ALARMA</button>

<div class="foot">AIRWATCH · Universidad de La Sabana · IoT 2026-1 · IBOCA / NIOSH REL</div>

<script>
// ── Chart PM2.5 + Temperatura ─────────────────────────────────────────────────
const chartPM = new Chart(document.getElementById('chartPM').getContext('2d'), {
  type: 'line',
  data: {
    labels: [],
    datasets: [
      {
        label: 'PM2.5',
        data: [],
        borderColor: '#e53e3e',
        backgroundColor: 'rgba(229,62,62,.06)',
        tension: 0.35,
        yAxisID: 'y',
        pointRadius: 1.5,
        borderWidth: 1.8
      },
      {
        label: 'Temp',
        data: [],
        borderColor: '#3182ce',
        backgroundColor: 'rgba(49,130,206,.06)',
        borderDash: [4, 3],
        tension: 0.35,
        yAxisID: 'y2',
        pointRadius: 1.5,
        borderWidth: 1.8
      }
    ]
  },
  options: {
    responsive: true,
    maintainAspectRatio: false,
    animation: false,
    plugins: { legend: { display: false } },
    scales: {
      x: {
        ticks: { maxTicksLimit: 6, font: { size: 9, family: "'IBM Plex Mono'" }, color: '#a0aec0' },
        grid: { color: 'rgba(226,232,240,.8)' }
      },
      y: {
        position: 'left',
        title: { display: true, text: 'PM2.5', font: { size: 9 }, color: '#e53e3e' },
        ticks: { font: { size: 9, family: "'IBM Plex Mono'" }, color: '#a0aec0' },
        grid: { color: 'rgba(226,232,240,.8)' }
      },
      y2: {
        position: 'right',
        title: { display: true, text: 'Temp °C', font: { size: 9 }, color: '#3182ce' },
        ticks: { font: { size: 9, family: "'IBM Plex Mono'" }, color: '#a0aec0' },
        grid: { drawOnChartArea: false }
      }
    }
  }
});

// ── Chart Gas ppm ─────────────────────────────────────────────────────────────
// El dataset índice 1 es la línea de umbral fija en 200 ppm (NIOSH REL TWA).
// Se inicializa vacía y se rellena con el histórico al cargar.
const chartGas = new Chart(document.getElementById('chartGas').getContext('2d'), {
  type: 'line',
  data: {
    labels: [],
    datasets: [
      {
        label: 'Gas ppm',
        data: [],
        borderColor: '#d69e2e',
        backgroundColor: 'rgba(214,158,46,.07)',
        tension: 0.35,
        yAxisID: 'y',
        pointRadius: 1.5,
        borderWidth: 1.8
      },
      {
        label: 'Límite NIOSH 200 ppm',
        data: [],
        borderColor: 'rgba(197,48,48,.5)',
        borderDash: [6, 4],
        borderWidth: 1.2,
        pointRadius: 0,
        yAxisID: 'y',
        fill: false,
        tension: 0
      }
    ]
  },
  options: {
    responsive: true,
    maintainAspectRatio: false,
    animation: false,
    plugins: { legend: { display: false } },
    scales: {
      x: {
        ticks: { maxTicksLimit: 6, font: { size: 9, family: "'IBM Plex Mono'" }, color: '#a0aec0' },
        grid: { color: 'rgba(226,232,240,.8)' }
      },
      y: {
        position: 'left',
        title: { display: true, text: 'ppm', font: { size: 9 }, color: '#d69e2e' },
        ticks: { font: { size: 9, family: "'IBM Plex Mono'" }, color: '#a0aec0' },
        grid: { color: 'rgba(226,232,240,.8)' }
      }
    }
  }
});

// ── Cargar histórico al iniciar ───────────────────────────────────────────────
fetch('/history')
  .then(r => r.json())
  .then(d => {
    // Gráfica PM2.5 + Temp
    chartPM.data.labels            = d.labels;
    chartPM.data.datasets[0].data  = d.pm25;
    chartPM.data.datasets[1].data  = d.temp;
    chartPM.update('none');
    // Gráfica Gas: datos reales + línea de umbral con misma longitud
    chartGas.data.labels           = d.labels.slice();
    chartGas.data.datasets[0].data = d.gas;
    chartGas.data.datasets[1].data = d.labels.map(() => 200);
    chartGas.update('none');
  })
  .catch(() => {});

// ── Polling cada 2 segundos ───────────────────────────────────────────────────
function poll() {
  fetch('/data')
    .then(r => {
      if (r.status === 401) { window.location = '/login'; throw 0; }
      if (!r.ok) throw 0;
      return r.json();
    })
    .then(d => {
      // Indicador de conectividad → verde
      const dot = document.getElementById('dot');
      dot.className = 'live-dot online';

      // Tarjetas de sensores
      setCard('pm25', d.pm25.toFixed(1), d.aPM25, '▲ SUPERA IBOCA NARANJA');
      setCard('pm10', d.pm10.toFixed(0),  false,   '');
      setCard('gas',  d.gas.toFixed(0),   d.aGas,  '▲ SUPERA NIOSH REL TWA');
      setCard('temp', d.temp.toFixed(1),  d.aTemp, '▲ FUERA DE RANGO');
      setCard('hum',  d.hum.toFixed(0),   d.aHum,  '▲ FUERA DE RANGO');
      setCard('pres', d.pres.toFixed(1),  d.aPres, '▲ FUERA DE RANGO');

      // Header — estado con variables específicas
      const hdr = document.getElementById('hdr');
      if (d.alarm) {
        hdr.className = 'hdr al';
        const vars = [];
        if (d.aPM25) vars.push('PM2.5');
        if (d.aGas)  vars.push('GAS');
        if (d.aTemp) vars.push('TEMP');
        if (d.aHum)  vars.push('HUM');
        if (d.aPres) vars.push('PRES');
        document.getElementById('estado').textContent = '▲ ALERTA: ' + vars.join(' · ');
      } else {
        hdr.className = 'hdr ok';
        document.getElementById('estado').textContent = '✓ CONDICIONES NORMALES';
      }

      // Timestamp
      const now = new Date();
      const hh = now.getHours().toString().padStart(2,'0');
      const mm = now.getMinutes().toString().padStart(2,'0');
      const ss = now.getSeconds().toString().padStart(2,'0');
      document.getElementById('ts').textContent = hh + ':' + mm + ':' + ss;
      const lbl = hh + ':' + mm + ':' + ss;

      // Botón de alarma
      const btnOff    = document.getElementById('btn-al');
      const btnEnable = document.getElementById('btn-enable');

      if (d.alarm) {
        // Alarma activa y sonando → mostrar botón de desactivar
        btnOff.style.display    = 'block';
        btnEnable.style.display = 'none';
      } else if (d.silenced) {
        // Silenciada por usuario pero aún por encima del umbral → mostrar re-habilitar
        btnOff.style.display    = 'none';
        btnEnable.style.display = 'block';
        // Header especial: condición anómala pero silenciada
        document.getElementById('hdr').className = 'hdr al';
        const vars = [];
        if (d.aPM25) vars.push('PM2.5');
        if (d.aGas)  vars.push('GAS');
        if (d.aTemp) vars.push('TEMP');
        if (d.aHum)  vars.push('HUM');
        if (d.aPres) vars.push('PRES');
        document.getElementById('estado').textContent = '⊘ SILENCIADA: ' + vars.join(' · ');
      } else {
        // Normal
        btnOff.style.display    = 'none';
        btnEnable.style.display = 'none';
      }

      // Actualizar gráfica PM2.5 + Temp (ventana 50)
      if (chartPM.data.labels.length >= 50) {
        chartPM.data.labels.shift();
        chartPM.data.datasets[0].data.shift();
        chartPM.data.datasets[1].data.shift();
      }
      chartPM.data.labels.push(lbl);
      chartPM.data.datasets[0].data.push(parseFloat(d.pm25.toFixed(1)));
      chartPM.data.datasets[1].data.push(parseFloat(d.temp.toFixed(1)));
      chartPM.update('none');

      // Actualizar gráfica Gas (ventana 50, umbral sigue igual)
      if (chartGas.data.labels.length >= 50) {
        chartGas.data.labels.shift();
        chartGas.data.datasets[0].data.shift();
        chartGas.data.datasets[1].data.shift();
      }
      chartGas.data.labels.push(lbl);
      chartGas.data.datasets[0].data.push(parseFloat(d.gas.toFixed(0)));
      chartGas.data.datasets[1].data.push(200); // Umbral fijo
      chartGas.update('none');
    })
    .catch(() => {
      // Indicador de conectividad → rojo (datos stale)
      document.getElementById('dot').className = 'live-dot offline';
    });
}

poll();
setInterval(poll, 2000);

// ── Helpers ───────────────────────────────────────────────────────────────────
function setCard(id, val, isAlert, msg) {
  document.getElementById('v-' + id).textContent = val;
  const card = document.getElementById('c-' + id);
  card.className = 'card ' + (isAlert ? 'al' : '');
  const w = document.getElementById('w-' + id);
  if (w) w.textContent = isAlert ? msg : '';
}

function apagar() {
  const btn = document.getElementById('btn-al');
  btn.disabled = true;
  btn.textContent = '... DESACTIVANDO';
  fetch('/alarm/off', { method: 'POST' })
    .then(r => {
      if (r.ok) {
        btn.style.display = 'none';
      } else {
        btn.disabled = false;
        btn.textContent = '▲ DESACTIVAR ALARMA FÍSICA';
      }
    })
    .catch(() => {
      btn.disabled = false;
      btn.textContent = '▲ DESACTIVAR ALARMA FÍSICA';
    });
}

function habilitar() {
  const btn = document.getElementById('btn-enable');
  btn.disabled = true;
  btn.textContent = '... HABILITANDO';
  fetch('/alarm/enable', { method: 'POST' })
    .then(r => {
      btn.disabled = false;
      btn.textContent = '↺ RE-HABILITAR ALARMA';
      if (!r.ok) console.warn('Error al re-habilitar');
    })
    .catch(() => {
      btn.disabled = false;
      btn.textContent = '↺ RE-HABILITAR ALARMA';
    });
}
</script>
</body></html>
)rawliteral";

// ─── Prototipos de funciones ──────────────────────────────────────────────────
// Auth helpers
String generateToken();
int    findSession(const String& token);
String createSession();
String getTokenFromRequest(AsyncWebServerRequest* request);
bool   isAuthenticated(AsyncWebServerRequest* request);
bool   safeCompare(const String& a, const String& b);

// Web handlers
void handleLoginPage(AsyncWebServerRequest* request);
void handleLoginPost(AsyncWebServerRequest* request);
void handleDashboard(AsyncWebServerRequest* request);
void handleHistory(AsyncWebServerRequest* request);
void handleAlarmOff(AsyncWebServerRequest* request);
void handleData(AsyncWebServerRequest* request);
void handleAlarmEnable(AsyncWebServerRequest* request);

// Sensor + display
void  buildSensorJSON(const DatosAmbientales& snap, char* buf, size_t bufSize);
float calcularPPM(float vout, float temp, float hum);
void  calibrarRo(int muestras);
void  mostrarVistasNormales(float t, float p, float pm25, float pm10, float g, float hum);
void  ejecutarAlarma(float pm25, float pm10, float g);

// FreeRTOS task
void measurementTask(void* pvParameters);

// Functions MQTT
void onMqttConnect(bool sessionPresent) {
  Serial.println("MQTT: Conectado al broker.");
  // Suscribirse al topic de comandos
  mqttClient.subscribe(MQTT_TOPIC_CMD, 1);
  Serial.printf("MQTT: Suscrito a %s\n", MQTT_TOPIC_CMD);
}

void onMqttMessage(char* topic, char* payload,
                   AsyncMqttClientMessageProperties properties,
                   size_t len, size_t index, size_t total) {
  // Copiar payload a un buffer terminado en null
  char buf[64];
  size_t copyLen = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
  memcpy(buf, payload, copyLen);
  buf[copyLen] = '\0';

  Serial.printf("MQTT CMD recibido [%s]: %s\n", topic, buf);

  // Parsear {"cmd":"alarm_off"}
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, buf) != DeserializationError::Ok) return;

  const char* cmd = doc["cmd"];
  if (cmd && strcmp(cmd, "alarm_off") == 0) {
    digitalWrite(ALARMA_PIN, LOW);
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      datos.alarmaActiva     = false;
      datos.alarmaSilenciada = true;
      xSemaphoreGive(dataMutex);
    }
    Serial.println("MQTT CMD: Alarma silenciada remotamente desde Ubidots.");
  }
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("MQTT: Desconectado. Reconectando en 5s...");
  if (WiFi.isConnected()) {
    xTaskCreatePinnedToCore(
      [](void*) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        mqttClient.connect();
        vTaskDelete(NULL);
      },
      "MQTTRecon", 4096, NULL, 1, NULL, 1
    );
  }
}

void publicarMQTT(float pm25, float pm10, float gas,
                  float temp, float hum,  float pres) {
  if (!mqttClient.connected()) return;

  char payload[160];
  snprintf(payload, sizeof(payload),
    "{\"pm25\":%.2f,\"pm10\":%.1f,\"gas\":%.1f,"
    "\"temp\":%.2f,\"hum\":%.1f,\"pres\":%.2f}",
    pm25, pm10, gas, temp, hum, pres
  );

  mqttClient.publish(MQTT_TOPIC, 1, false, payload);
  Serial.printf("MQTT: publicado → %s\n", payload);
}

// =============================================================================
// mqttTask — Hilo de transmisión MQTT (núcleo 0)
// Recibe datos de measurementTask por cola y los publica al broker.
// =============================================================================
void mqttTask(void* pvParameters) {
  DatosAmbientales d;
  for (;;) {
    // Bloquea hasta recibir un dato de measurementTask
    if (xQueueReceive(mqttQueue, &d, portMAX_DELAY)) {
      if (!mqttClient.connected()) {
        Serial.println("MQTT: sin conexión, dato descartado.");
        continue;
      }
      char payload[160];
      snprintf(payload, sizeof(payload),
        "{\"pm25\":%.2f,\"pm10\":%.1f,\"gas\":%.1f,"
        "\"temp\":%.2f,\"hum\":%.1f,\"pres\":%.2f,"
        "\"alarma\":%d}",
        d.pm25, d.pm10, d.gasPPM,
        d.temp, d.hum, d.pres,
        d.alarmaActiva ? 1 : 0
      );
      mqttClient.publish(MQTT_TOPIC, 1, false, payload);
      Serial.printf("MQTT: publicado → %s\n", payload);
    }
  }
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Iniciando...");

  dht.begin();
  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  pms.passiveMode();
  pinMode(ALARMA_PIN, OUTPUT);
  digitalWrite(ALARMA_PIN, LOW);

  if (!bmp.begin(0x76)) {
    Serial.println("ERROR: BMP280 no detectado.");
    lcd.clear(); lcd.setCursor(0,0); lcd.print("ERROR BMP280");
    while (1);
  }

  for (int i = 0; i < VENTANA; i++) { pmBuffer[i] = 0; gasBuffer[i] = 0; }

  Serial.println("Calibrando MQ-135 en aire limpio (24 muestras)...");
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Calibrando gas..");
  calibrarRo(24);
  Serial.printf("Ro calibrado: %.0f Ω\n", Ro);

  lcd.clear(); lcd.setCursor(0,0); lcd.print("Conectando WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 20) {
    delay(500); Serial.print("."); intentos++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWi-Fi conectado. IP: %s\n", WiFi.localIP().toString().c_str());
    lcd.clear(); lcd.setCursor(0,0); lcd.print("IP:");
    lcd.setCursor(0,1); lcd.print(WiFi.localIP().toString());
    delay(3000);
  } else {
    Serial.println("\nERROR: No se pudo conectar al Wi-Fi.");
    Serial.println(WiFi.status());
    lcd.clear(); lcd.setCursor(0,0); lcd.print("Sin WiFi");
    lcd.setCursor(0,1); lcd.print("Solo local");
    delay(3000);
  }

  // ─── Registrar rutas ──────────────────────────────────────────────────────
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    handleDashboard(request);
  });
  server.on("/login", HTTP_GET, [](AsyncWebServerRequest* request) {
    handleLoginPage(request);
  });
  server.on("/login", HTTP_POST, [](AsyncWebServerRequest* request) {
    handleLoginPost(request);
  });
  server.on("/history", HTTP_GET, [](AsyncWebServerRequest* request) {
    handleHistory(request);
  });
  server.on("/alarm/off", HTTP_POST, [](AsyncWebServerRequest* request) {
    handleAlarmOff(request);
  });
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest* request) {
    handleData(request);
  });
  server.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "No encontrado");
  });

  server.on("/alarm/enable", HTTP_POST, [](AsyncWebServerRequest* request) {
    handleAlarmEnable(request);
  });

  // ─── MQTT ─────────────────────────────────────────────────────────────────
  mqttQueue = xQueueCreate(5, sizeof(DatosAmbientales));

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCredentials(MQTT_USER, MQTT_PASS);
  mqttClient.setClientId(MQTT_CLIENT);

  xTaskCreatePinnedToCore(mqttTask, "MQTTTask", 4096, NULL, 1,
                          &mqttTaskHandle, 1);

  xTaskCreatePinnedToCore(
    [](void*) {
      vTaskDelay(pdMS_TO_TICKS(500));
      if (WiFi.status() == WL_CONNECTED) mqttClient.connect();
      vTaskDelete(NULL);
    },
    "MQTTConnect", 4096, NULL, 1, NULL, 1
  );

  server.begin();
  Serial.println("Servidor web asíncrono iniciado en puerto 80.");

  dataMutex = xSemaphoreCreateMutex();
  if (dataMutex == NULL) {
    Serial.println("ERROR FATAL: No se pudo crear el mutex.");
    while (1);
  }

  xTaskCreatePinnedToCore(measurementTask, "Medicion", 16384, NULL, 2,
                          &measurementTaskHandle, 0);

  Serial.println("=== SISTEMA DE CALIDAD DE AIRE INICIADO ===");
  Serial.printf("ESP32 Arduino Core: %s\n", ESP.getSdkVersion());
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
  vTaskDelete(NULL);
}

// =============================================================================
// measurementTask — Hilo de medición y periféricos (núcleo 0)
// =============================================================================
void measurementTask(void* pvParameters) {
  for (;;) {
    // 1. BMP280
    float pres = bmp.readPressure() / 100.0F;
    float temp = bmp.readTemperature();
    if (isnan(temp) || temp < -40) {
      temp = dht.readTemperature();
      Serial.println("SISTEMA: Usando temperatura de respaldo (DHT11)");
    }

    // 2. DHT11
    float hum = dht.readHumidity();
    if (isnan(hum)) { hum = 50.0; Serial.println("ERROR DHT11: usando 50%."); }

    // 3. PMS5003
    float pm25_raw = 0, pm10_raw = 0;
    bool  pmsOk = false;
    pms.requestRead();
    if (pms.readUntil(pmsData, 2000)) {
      pm25_raw = pmsData.PM_AE_UG_2_5;
      pm10_raw = pmsData.PM_AE_UG_10_0;
      pmsOk    = true;
    }

    // 4. MQ-135
    float vout   = analogRead(MQ135_PIN) * (3.3f / 4095.0f);
    float gasPPM = calcularPPM(vout, temp, hum);

    if (!pmsOk) {
      Serial.println("ADVERTENCIA: Sin datos del PMS5003. Reintentando...");
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Esperando PMS...");
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    // 5. Corrección Barkjohn (EPA, 2021)
    float pm25_corr = 0.541f * pm25_raw - 0.0618f * hum + 0.00534f * temp + 3.634f;
    if (pm25_corr < 0) pm25_corr = 0;
    float pm10_info = pm10_raw;

    // 6. Buffer promedio móvil
    pmBuffer[mvIdx]  = pm25_corr;
    gasBuffer[mvIdx] = gasPPM;
    mvIdx = (mvIdx + 1) % VENTANA;
    if (mvCount < VENTANA) mvCount++;

    Serial.println("\n--- LECTURA ACTUAL ---");
    Serial.printf("T: %.1f C | H: %.1f %% | Pres: %.1f hPa\n", temp, hum, pres);
    Serial.printf("PM2.5 crudo: %.1f -> corregido: %.1f ug/m3\n", pm25_raw, pm25_corr);
    Serial.printf("Gas: %.1f ppm\n", gasPPM);

    if (mvCount >= VENTANA) {
      // 7. Promedios móviles
      float promPM25 = 0, promGas = 0;
      for (int i = 0; i < VENTANA; i++) { promPM25 += pmBuffer[i]; promGas += gasBuffer[i]; }
      promPM25 /= VENTANA;
      promGas  /= VENTANA;

      // 8. Evaluar umbrales
      bool aPM25  = promPM25 > PM25_UMBRAL;
      bool aGas   = promGas  > GAS_UMBRAL;
      bool aTemp  = (temp < TEMP_MIN || temp > TEMP_MAX);
      bool aHum   = (hum  < HUM_MIN  || hum  > HUM_MAX);
      bool aPres  = (pres < PRES_MIN  || pres > PRES_MAX);
      // Leer el flag de silencio antes de decidir si activar
      bool hayAlerta = aPM25 || aGas || aTemp || aHum || aPres;

      // Leer alarmaSilenciada bajo mutex (snapshot rápido)
      bool silenciada = false;
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        silenciada = datos.alarmaSilenciada;
        xSemaphoreGive(dataMutex);
      }

      // Si sigue por encima del umbral pero el usuario silenció → no reactivar
      // Si las condiciones vuelven a la normalidad → limpiar el silencio automáticamente
      bool alarma = hayAlerta && !silenciada;

      // Auto-reset: si ya no hay alerta, liberar el silencio para que la
      // próxima alerta real notifique de nuevo sin intervención del usuario
      if (!hayAlerta && silenciada) {
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          datos.alarmaSilenciada = false;
          xSemaphoreGive(dataMutex);
        }
        silenciada = false;
      }

      Serial.printf(">>> ESTADO: %s <<<\n", alarma ? "ALARMA ACTIVA" : "NORMAL");

      // 9. Actualizar datos compartidos + histórico bajo mutex
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        datos.temp         = temp;
        datos.hum          = hum;
        datos.pres         = pres;
        datos.pm25         = promPM25;
        datos.pm10         = pm10_info;
        datos.gasPPM       = promGas;
        datos.alarmaActiva = alarma;
        datos.datosListos  = true;
        datos.alertaPM25   = aPM25;
        datos.alertaGas    = aGas;
        datos.alertaTemp   = aTemp;
        datos.alertaHum    = aHum;
        datos.alertaPres   = aPres;

        historyBuf[historyIdx] = { promPM25, pm10_info, promGas, temp, hum, pres };
        historyIdx = (historyIdx + 1) % HISTORY_SIZE;
        if (historyCount < HISTORY_SIZE) historyCount++;

        xSemaphoreGive(dataMutex);
      }

      // Enviar snapshot a mqttTask por cola (no bloquea la medición)
      DatosAmbientales snapshot;
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snapshot = datos;
        xSemaphoreGive(dataMutex);
      }
      xQueueSend(mqttQueue, &snapshot, 0);

      // 10. Periféricos
      if (alarma) {
        ejecutarAlarma(promPM25, pm10_info, promGas);
      } else {
        digitalWrite(ALARMA_PIN, LOW);
        mostrarVistasNormales(temp, pres, promPM25, pm10_info, promGas, hum);
      }

    } else {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Acumulando...");
      lcd.setCursor(0,1); lcd.print(mvCount); lcd.print(" / "); lcd.print(VENTANA);
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// =============================================================================
// AUTH HELPERS
// =============================================================================

/**
 * generateToken — Genera token hex de 16 chars con RNG hardware (TRNG ESP32).
 * esp_random() es criptográficamente seguro cuando el radio Wi-Fi está activo.
 */
String generateToken() {
  char buf[17];
  snprintf(buf, sizeof(buf), "%08X%08X", esp_random(), esp_random());
  return String(buf);
}

/**
 * findSession — Busca sesión activa y no expirada. Retorna índice o -1.
 */
int findSession(const String& token) {
  uint32_t now = millis();
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].active &&
        strcmp(sessions[i].token, token.c_str()) == 0 &&
        sessions[i].expiry > now) {
      return i;
    }
  }
  return -1;
}

/**
 * createSession — Crea sesión nueva. Si no hay slots libres, usa el más viejo.
 */
String createSession() {
  String   token  = generateToken();
  uint32_t expiry = millis() + SESSION_MS;

  int      slot   = 0;
  uint32_t oldest = UINT32_MAX;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].active) { slot = i; break; }
    if (sessions[i].expiry < oldest) { oldest = sessions[i].expiry; slot = i; }
  }

  strncpy(sessions[slot].token, token.c_str(), 16);
  sessions[slot].token[16] = '\0';
  sessions[slot].expiry    = expiry;
  sessions[slot].active    = true;
  return token;
}

/**
 * getTokenFromRequest — Extrae cookie "session" del header HTTP.
 */
String getTokenFromRequest(AsyncWebServerRequest* request) {
  if (!request->hasHeader("Cookie")) return "";
  String cookie = request->header("Cookie");
  int pos = cookie.indexOf("session=");
  if (pos < 0) return "";
  pos += 8;
  int end = cookie.indexOf(';', pos);
  if (end < 0) end = cookie.length();
  return cookie.substring(pos, end);
}

/**
 * isAuthenticated — Verifica token de sesión válido en la cookie.
 */
bool isAuthenticated(AsyncWebServerRequest* request) {
  String token = getTokenFromRequest(request);
  if (token.isEmpty()) return false;
  return findSession(token) >= 0;
}

/**
 * safeCompare — Comparación en tiempo constante para evitar timing attacks.
 * Documentado como limitación de prototipo: no usa memoria segura (SecureZeroMemory).
 */
bool safeCompare(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  bool match = true;
  for (size_t i = 0; i < a.length(); i++) {
    if (a[i] != b[i]) match = false;
  }
  return match;
}

// =============================================================================
// WEB HANDLERS
// =============================================================================

void handleLoginPage(AsyncWebServerRequest* request) {
  if (isAuthenticated(request)) { request->redirect("/"); return; }
  String html = FPSTR(LOGIN_HTML);
  html.replace("%ERR_CLASS%", "");
  html.replace("%ERR_MSG%",   "");
  request->send(200, "text/html; charset=utf-8", html);
}

void handleLoginPost(AsyncWebServerRequest* request) {
  String user = "", pass = "";
  if (request->hasParam("user", true)) user = request->getParam("user", true)->value();
  if (request->hasParam("pass", true)) pass = request->getParam("pass", true)->value();

  UserAccount* account = nullptr;
  for (int i = 0; i < NUM_ACCOUNTS; i++) {
    if (safeCompare(user, String(accounts[i].user))) { account = &accounts[i]; break; }
  }

  // Usuario inexistente → misma respuesta que contraseña incorrecta (no revelar existencia)
  if (account == nullptr) {
    String html = FPSTR(LOGIN_HTML);
    html.replace("%ERR_CLASS%", "show");
    html.replace("%ERR_MSG%",   "Usuario o contrasena incorrectos.");
    request->send(401, "text/html; charset=utf-8", html);
    return;
  }

  // Bloqueo por intentos fallidos
  if (account->lockoutUntil != 0 &&
      (millis() - account->lockoutUntil) < LOCKOUT_MS) {
    String html = FPSTR(LOGIN_HTML);
    html.replace("%ERR_CLASS%", "show");
    html.replace("%ERR_MSG%",   "Cuenta bloqueada. Espere 5 minutos.");
    request->send(429, "text/html; charset=utf-8", html);
    return;
  }

  if (safeCompare(pass, String(account->pass))) {
    account->loginAttempts = 0;
    account->lockoutUntil  = 0;
    String token = createSession();
    AsyncWebServerResponse* resp = request->beginResponse(302, "text/plain", "OK");
    resp->addHeader("Location", "/");
    resp->addHeader("Set-Cookie",
      "session=" + token + "; HttpOnly; Path=/; Max-Age=" + String(SESSION_MS / 1000));
    request->send(resp);
    Serial.printf("AUTH: Login exitoso — usuario '%s'\n", account->user);
  } else {
    account->loginAttempts++;
    Serial.printf("AUTH: Intento fallido '%s' (%d/%d)\n",
                  account->user, account->loginAttempts, MAX_LOGIN_ATTEMPTS);
    if (account->loginAttempts >= MAX_LOGIN_ATTEMPTS) {
      account->lockoutUntil  = millis();
      account->loginAttempts = 0;
      Serial.printf("AUTH: '%s' bloqueado por 5 minutos.\n", account->user);
    }
    String html = FPSTR(LOGIN_HTML);
    html.replace("%ERR_CLASS%", "show");
    html.replace("%ERR_MSG%",   "Usuario o contrasena incorrectos.");
    request->send(401, "text/html; charset=utf-8", html);
  }
}

void handleDashboard(AsyncWebServerRequest* request) {
  if (!isAuthenticated(request)) { request->redirect("/login"); return; }
  request->send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
}

/**
 * handleHistory — JSON con últimas HISTORY_SIZE lecturas.
 * Formato: { "labels":[...], "pm25":[...], "temp":[...], "gas":[...] }
 */
void handleHistory(AsyncWebServerRequest* request) {
  if (!isAuthenticated(request)) {
    request->send(401, "text/plain", "No autorizado");
    return;
  }

  HistoryEntry snap[HISTORY_SIZE];
  int snapCount = 0, snapIdx = 0;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    snapCount = historyCount;
    snapIdx   = historyIdx;
    memcpy(snap, historyBuf, sizeof(HistoryEntry) * HISTORY_SIZE);
    xSemaphoreGive(dataMutex);
  } else {
    request->send(503, "application/json", "{\"error\":\"busy\"}");
    return;
  }

  // Cálculo de tamaño:
  // labels: "0"-"49" → ~5×50=250 | pm25: ~7×50=350 | temp: ~6×50=300
  // gas:  hasta "9999.0," → ~8×50=400 | overhead ~100 → total ~1400
  static char jsonBuf[1600];
  int total = min(snapCount, HISTORY_SIZE);
  int pos   = 0;

  pos += snprintf(jsonBuf + pos, sizeof(jsonBuf) - pos, "{\"labels\":[");
  for (int i = 0; i < total; i++)
    pos += snprintf(jsonBuf + pos, sizeof(jsonBuf) - pos, i ? ",\"%d\"" : "\"%d\"", i);

  pos += snprintf(jsonBuf + pos, sizeof(jsonBuf) - pos, "],\"pm25\":[");
  for (int i = 0; i < total; i++) {
    int ri = (snapIdx - total + i + HISTORY_SIZE) % HISTORY_SIZE;
    pos += snprintf(jsonBuf + pos, sizeof(jsonBuf) - pos, i ? ",%.1f" : "%.1f", snap[ri].pm25);
  }

  pos += snprintf(jsonBuf + pos, sizeof(jsonBuf) - pos, "],\"temp\":[");
  for (int i = 0; i < total; i++) {
    int ri = (snapIdx - total + i + HISTORY_SIZE) % HISTORY_SIZE;
    pos += snprintf(jsonBuf + pos, sizeof(jsonBuf) - pos, i ? ",%.1f" : "%.1f", snap[ri].temp);
  }

  // Campo gas — necesario para la segunda gráfica del dashboard
  pos += snprintf(jsonBuf + pos, sizeof(jsonBuf) - pos, "],\"gas\":[");
  for (int i = 0; i < total; i++) {
    int ri = (snapIdx - total + i + HISTORY_SIZE) % HISTORY_SIZE;
    pos += snprintf(jsonBuf + pos, sizeof(jsonBuf) - pos, i ? ",%.1f" : "%.1f", snap[ri].gasPPM);
  }

  pos += snprintf(jsonBuf + pos, sizeof(jsonBuf) - pos, "]}");

  request->send(200, "application/json", jsonBuf);
}

void handleAlarmOff(AsyncWebServerRequest* request) {
  if (!isAuthenticated(request)) {
    request->send(401, "text/plain", "No autorizado");
    return;
  }
  digitalWrite(ALARMA_PIN, LOW);
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    datos.alarmaActiva     = false;
    datos.alarmaSilenciada = true;   // ← bloquea reactivación automática
    xSemaphoreGive(dataMutex);
  }
  Serial.println("WEB: Alarma silenciada por usuario.");
  request->send(200, "text/plain", "OK");
}

/**
 * handleData — Snapshot de la lectura más reciente en JSON.
 * Llamado cada 2 s por el polling del dashboard.
 */
void handleData(AsyncWebServerRequest* request) {
  if (!isAuthenticated(request)) {
    request->send(401, "text/plain", "No autorizado");
    return;
  }

  DatosAmbientales snap = {};
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    snap = datos;
    xSemaphoreGive(dataMutex);
  } else {
    request->send(503, "application/json", "{\"error\":\"busy\"}");
    return;
  }

  char buf[220];
  buildSensorJSON(snap, buf, sizeof(buf));

  AsyncWebServerResponse* resp =
    request->beginResponse(200, "application/json", buf);
  resp->addHeader("Cache-Control", "no-store");
  request->send(resp);
}

void handleAlarmEnable(AsyncWebServerRequest* request) {
  if (!isAuthenticated(request)) {
    request->send(401, "text/plain", "No autorizado");
    return;
  }
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    datos.alarmaSilenciada = false;
    xSemaphoreGive(dataMutex);
  }
  Serial.println("WEB: Alarma re-habilitada por usuario.");
  request->send(200, "text/plain", "OK");
}

// =============================================================================
// buildSensorJSON — Serializa DatosAmbientales a JSON
// =============================================================================
void buildSensorJSON(const DatosAmbientales& s, char* buf, size_t bufSize) {
  snprintf(buf, bufSize,
    "{\"pm25\":%.2f,\"pm10\":%.1f,\"gas\":%.1f,"
    "\"temp\":%.2f,\"hum\":%.1f,\"pres\":%.2f,"
    "\"alarm\":%s,\"silenced\":%s,"
    "\"aPM25\":%s,\"aGas\":%s,"
    "\"aTemp\":%s,\"aHum\":%s,\"aPres\":%s}",
    s.pm25, s.pm10, s.gasPPM,
    s.temp, s.hum,  s.pres,
    s.alarmaActiva     ? "true" : "false",
    s.alarmaSilenciada ? "true" : "false",
    s.alertaPM25   ? "true" : "false",
    s.alertaGas    ? "true" : "false",
    s.alertaTemp   ? "true" : "false",
    s.alertaHum    ? "true" : "false",
    s.alertaPres   ? "true" : "false"
  );
}

// =============================================================================
// FUNCIONES DE SENSORES
// =============================================================================

float calcularPPM(float vout, float temp, float hum) {
  if (vout < 0.01f)  return 0.0f;
  if (vout >= 3.29f) return 9999.0f;
  float sensorRs  = RL * (3.3f - vout) / vout;
  float corrFactor = -0.00268f * temp + 0.01383f * hum + 1.2698f;
  if (corrFactor < 0.1f) corrFactor = 0.1f;
  float Rs_corr = sensorRs / corrFactor;
  float ratio   = Rs_corr / Ro;
  if (ratio <= 0) return 0.0f;
  float ppm = PARA_A * pow(ratio, PARA_B);
  return ppm < 0 ? 0 : ppm;
}

void calibrarRo(int muestras) {
  float sumaRs = 0;
  for (int i = 0; i < muestras; i++) {
    float vout = analogRead(MQ135_PIN) * (3.3f / 4095.0f);
    if (vout > 0.01f) {
      sumaRs += RL * (3.3f - vout) / vout;
    }
    delay(500); 
  }
  float promRs = sumaRs / muestras;

  // En aire limpio, el ratio Rs/Ro es aproximadamente 3.6
  // Por lo tanto: Ro = Rs_aire_limpio / 3.6
  Ro = promRs / 3.6; 
}

void mostrarVistasNormales(float t, float p, float pm25,
                            float pm10, float g, float hum) {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("T:"); lcd.print(t,1);
  lcd.print("C H:"); lcd.print(hum,0); lcd.print("%");
  lcd.setCursor(0,1); lcd.print("P:"); lcd.print(p,0); lcd.print("hPa");
  vTaskDelay(pdMS_TO_TICKS(5000));

  lcd.clear();
  lcd.setCursor(0,0); lcd.print("2.5:"); lcd.print(pm25,1);
  lcd.print(" 10:"); lcd.print(pm10,0);
  lcd.setCursor(0,1); lcd.print("Gas:"); lcd.print(g,0); lcd.print("ppm OK");
  vTaskDelay(pdMS_TO_TICKS(5000));
}

void ejecutarAlarma(float pm25, float pm10, float g) {
  digitalWrite(ALARMA_PIN, HIGH);
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("!!! ALERTA !!!");
  lcd.setCursor(0,1); lcd.print("AIRE TOXICO");
  vTaskDelay(pdMS_TO_TICKS(3000));
  digitalWrite(ALARMA_PIN, LOW);
}
