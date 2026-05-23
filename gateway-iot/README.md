# AirWatch Sabana — Sistema de Monitoreo de Calidad del Aire

Sistema IoT de bajo costo para monitorear la calidad del aire en la región Sabana Centro (Cundinamarca). Mide PM2.5, PM10, gases contaminantes, temperatura, humedad y presión atmosférica.

**Universidad de La Sabana · IoT 2026-1**

---

## Estructura del repositorio

```
.
├── dispositivo-iot
│   └── sistema_calidad_aire
│       └── sistema_calidad_aire.ino   ← Firmware ESP32
├── gateway-iot
│   ├── data/                          ← Volumen SQLite (generado automáticamente)
│   ├── docker-compose.yml             ← Definición de servicios
│   ├── logger/
│   │   ├── Dockerfile
│   │   └── main.py                    ← Suscriptor MQTT → SQLite
│   └── mosquitto/
│       └── config/
│           └── mosquitto.conf         ← Configuración del broker
└── README.md
```

---

## Requisitos previos

| Herramienta | Versión mínima |
|---|---|
| Docker + Docker Compose | 24.x |
| Git | 2.x |
| Arduino IDE | 2.x (para el firmware del ESP32) |

---

## Puesta en marcha del gateway

### 1. Clonar el repositorio

```bash
git clone https://github.com/Kanpoz/Proyecto-IoT-Grupo-7.git
cd Proyecto-IoT-Grupo-7/gateway-iot
```

### 2. Crear el archivo `.env`

El archivo `.env` **no se incluye en el repositorio** por seguridad. Créalo en `gateway-iot/` con las credenciales que te envíen por un canal seguro:

```bash
nano .env
```

Contenido esperado:

```env
MQTT_USER=admin
MQTT_PASS=tu_contraseña_segura
MQTT_CAMPOS_ESPERADOS=pm25,pm10,gas,temp,hum,pres
```

### 3. Restaurar el archivo `pwfile` de Mosquitto

El archivo `pwfile` **no se incluye en el repositorio**. Tienes dos opciones:

**Opción A — Te lo envían directamente:**
Copia el archivo recibido a su ubicación:

```bash
cp /ruta/al/pwfile mosquitto/config/pwfile
chmod 644 mosquitto/config/pwfile
```

**Opción B — Generarlo desde cero** (necesitas saber las contraseñas de cada dispositivo):

```bash
# Crear el archivo vacío con permisos correctos
touch mosquitto/config/pwfile
chmod 644 mosquitto/config/pwfile

# Agregar el usuario del gateway
docker run --rm -it \
  -v ./mosquitto/config:/mosquitto/config \
  eclipse-mosquitto \
  mosquitto_passwd /mosquitto/config/pwfile admin

# Agregar cada dispositivo ESP32
docker run --rm -it \
  -v ./mosquitto/config:/mosquitto/config \
  eclipse-mosquitto \
  mosquitto_passwd /mosquitto/config/pwfile esp32_01
```

### 4. Levantar los servicios

```bash
docker compose up -d --build
```

### 5. Verificar que todo está corriendo

```bash
docker compose ps
```

Deberías ver los tres contenedores con estado `running`:

```
NAME           STATUS
mosquitto      Up
data-logger    Up
```

Verificar los logs:

```bash
docker compose logs -f
```

Líneas esperadas en los logs:

```
mosquitto   | mosquitto version 2.x starting
mosquitto   | Config loaded from /mosquitto/config/mosquitto.conf
data-logger | Base de datos lista: /data/sensores.db
data-logger | Conectado al broker MQTT (mosquitto:1883)
data-logger | Suscrito a: dispositivos/+/datos
```

---

## Agregar un nuevo dispositivo ESP32

Cada ESP32 necesita su propio usuario en Mosquitto:

```bash
docker exec -it mosquitto \
  mosquitto_passwd /mosquitto/config/pwfile esp32_02

docker compose restart mosquitto
```

Y actualizar el firmware del nuevo ESP32 con sus credenciales correspondientes (ver sección de dispositivo IoT más abajo).

---

## Probar el flujo sin el ESP32

Para verificar que el gateway funciona correctamente sin necesidad del hardware:

```bash
# Publicar un mensaje de prueba
docker exec -it mosquitto mosquitto_pub \
  -h localhost \
  -t "dispositivos/esp32_01/datos" \
  -m '{"pm25":12.3,"pm10":18.0,"gas":95.5,"temp":22.1,"hum":55.0,"pres":748.2}' \
  -u admin \
  -P tu_contraseña

# Verificar que quedó guardado en SQLite
docker exec -it data-logger \
  python -c "import sqlite3; c=sqlite3.connect('/data/sensores.db'); \
  [print(r) for r in c.execute('SELECT * FROM lecturas ORDER BY id DESC LIMIT 5')]"
```

---

## Puesta en marcha del dispositivo IoT (ESP32)

### Requisitos de hardware

| Componente | Interfaz |
|---|---|
| ESP32 DevKit | — |
| PMS5003 (sensor PM) | UART2 — RX:GPIO16, TX:GPIO17 |
| BMP280 (presión/temperatura) | I2C — SDA:GPIO21, SCL:GPIO22, dir:0x76 |
| DHT11 (humedad/temperatura) | Digital — GPIO27 |
| MQ-135 (gases) | ADC — GPIO34 |
| LCD 16x2 I2C | I2C — dir:0x27 |
| LED/Buzzer | Digital — GPIO13 |

### Librerías Arduino requeridas

Instalar desde el Gestor de Librerías del Arduino IDE:

- `LiquidCrystal I2C`
- `Adafruit BMP280`
- `Adafruit Unified Sensor`
- `DHT sensor library` (Adafruit)
- `PMS Library` (Mariusz Kacki)
- `ESPAsyncWebServer`
- `AsyncTCP`
- `AsyncMqttClient`

### Configuración del firmware

Antes de compilar, editar las siguientes constantes en `sistema_calidad_aire.ino`:

```cpp
// Wi-Fi
const char* WIFI_SSID     = "NOMBRE_DE_TU_RED";
const char* WIFI_PASSWORD = "CONTRASEÑA_WIFI";

// MQTT — IP del gateway en la red local
#define MQTT_HOST  IPAddress(192, 168, X, X)   // IP de la PC o Raspberry Pi
#define MQTT_USER  "esp32_01"                   // usuario registrado en Mosquitto
#define MQTT_PASS  "contraseña_del_dispositivo"
```

### Compilar y cargar

1. Abrir `dispositivo-iot/sistema_calidad_aire/sistema_calidad_aire.ino` en Arduino IDE
2. Seleccionar la placa: **ESP32 Dev Module**
3. Seleccionar el puerto COM/USB correspondiente
4. Compilar y cargar (`Ctrl+U`)
5. Abrir el monitor serial a **115200 baudios** para verificar la salida

Salida esperada en el monitor serial:

```
Calibrando MQ-135 en aire limpio (24 muestras)...
Ro calibrado: XXXXX Ω
Wi-Fi conectado. IP: 192.168.X.X
Servidor web asíncrono iniciado en puerto 80.
=== SISTEMA DE CALIDAD DE AIRE INICIADO ===
MQTT: Conectado al broker.
MQTT: publicado → {"pm25":0.10,"pm10":0.0,"gas":45.2,"temp":23.1,"hum":58.0,"pres":748.1}
```

### Acceder al dashboard local

Con el ESP32 conectado a la red, abrir un navegador y acceder a la IP que aparece en el monitor serial:

```
http://192.168.X.X
```

Credenciales de acceso:

| Usuario | Perfil |
|---|---|
| `admin` | Administrador |
| `alcaldia` | Personal alcaldía |
| `tecnico` | Técnico de campo |

> Las contraseñas se comparten por canal seguro.

---

## Despliegue en Raspberry Pi 5

```bash
# Instalar Docker (solo la primera vez)
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
newgrp docker

# Clonar el repositorio
git clone https://github.com/Kanpoz/Proyecto-IoT-Grupo-7.git
cd Proyecto-IoT-Grupo-7/gateway-iot

# Copiar .env y pwfile recibidos por canal seguro
cp /ruta/.env .env
cp /ruta/pwfile mosquitto/config/pwfile
chmod 644 mosquitto/config/pwfile

# Levantar (Docker detecta automáticamente la arquitectura ARM64)
docker compose up -d --build
```

---

## Comandos útiles

```bash
# Ver estado de los contenedores
docker compose ps

# Ver logs en tiempo real
docker compose logs -f

# Ver solo logs de un servicio
docker compose logs -f mosquitto
docker compose logs -f data-logger

# Detener todos los servicios
docker compose down

# Reiniciar un servicio específico
docker compose restart mosquitto

# Consultar las últimas 10 lecturas en SQLite
docker exec -it data-logger \
  python -c "import sqlite3; c=sqlite3.connect('/data/sensores.db'); \
  [print(r) for r in c.execute('SELECT * FROM lecturas ORDER BY id DESC LIMIT 10')]"
```

---

## Arquitectura de hilos — ESP32

| Hilo | Núcleo | Responsabilidad |
|---|---|---|
| `loop()` | 1 | Vacío — eliminado con `vTaskDelete(NULL)` |
| `measurementTask` | 0 | Lectura de sensores, promedios, alertas, LCD |
| `mqttTask` | 1 | Transmisión MQTT al gateway vía cola FreeRTOS |
| `async_tcp` | 1 | Servidor web embebido |

La medición y la transmisión están desacopladas mediante una cola FreeRTOS — si el broker no está disponible, los sensores siguen midiendo sin interrupciones.

---

## Flujo de datos

```
ESP32 (sensores)
  └── measurementTask (núcleo 0)
        └── cola FreeRTOS
              └── mqttTask (núcleo 1)
                    └── MQTT → Mosquitto :1883
                          └── data-logger (Python)
                                └── SQLite /data/sensores.db
```

---

## Uso de IA en el desarrollo

Este proyecto utilizó **Claude (Anthropic)** como asistente de desarrollo. Ver la sección de IA en la Wiki del proyecto para el detalle de consultas, validaciones y aplicación específica a la solución.
