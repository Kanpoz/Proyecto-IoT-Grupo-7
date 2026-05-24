"""
data-logger
==============
Suscriptor MQTT que persiste lecturas de sensores IoT en SQLite.

Flujo:
  ESP32  →  MQTT topic: dispositivos/<device_id>/datos
         →  JSON payload: {"temp": 22.5, "hum": 60, ...}
         →  SQLite: tabla lecturas

Variables de entorno:
  MQTT_HOST   Host del broker     (default: mosquitto)
  MQTT_PORT   Puerto del broker   (default: 1883)
  MQTT_USER   Usuario MQTT
  MQTT_PASS   Contraseña MQTT
  MQTT_TOPIC  Topic a suscribir   (default: dispositivos/+/datos)
  DB_PATH     Ruta de la DB       (default: /data/sensores.db)
"""

import os
import time
import json
import sqlite3
import logging

import paho.mqtt.client as mqtt

# =============================================================================
# Configuración desde variables de entorno
# =============================================================================

MQTT_HOST  = os.getenv("MQTT_HOST",  "mosquitto")
MQTT_PORT  = int(os.getenv("MQTT_PORT", "1883"))
MQTT_USER  = os.getenv("MQTT_USER")
MQTT_PASS  = os.getenv("MQTT_PASS")
MQTT_TOPIC = os.getenv("MQTT_TOPIC", "dispositivos/+/datos")
DB_PATH    = os.getenv("DB_PATH",    "/data/sensores.db")

RECONNECT_DELAY = 5   # segundos entre intentos de reconexión

# =============================================================================
# Logging
# =============================================================================

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger(__name__)

# =============================================================================
# Base de datos
# =============================================================================

def init_db(path: str) -> sqlite3.Connection:
    """
    Crea la conexión y la tabla si no existe.
    Incluye device_id como columna dedicada para facilitar consultas futuras.
    """
    con = sqlite3.connect(path, check_same_thread=False)
    con.execute("PRAGMA journal_mode=WAL")   # mejor rendimiento con escrituras concurrentes
    con.execute("""
        CREATE TABLE IF NOT EXISTS lecturas (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT    NOT NULL,
            topic     TEXT    NOT NULL,
            payload   TEXT    NOT NULL,
            timestamp DATETIME DEFAULT (strftime('%Y-%m-%dT%H:%M:%S', 'now'))
        )
    """)
    # Índice para consultas por dispositivo o rango de tiempo
    con.execute("""
        CREATE INDEX IF NOT EXISTS idx_device_ts
        ON lecturas (device_id, timestamp)
    """)
    con.commit()
    log.info("Base de datos lista: %s", path)
    return con


def insertar_lectura(con: sqlite3.Connection, device_id: str, topic: str, payload: str):
    """Inserta una fila en la tabla lecturas."""
    con.execute(
        "INSERT INTO lecturas (device_id, topic, payload) VALUES (?, ?, ?)",
        (device_id, topic, payload),
    )
    con.commit()

# =============================================================================
# Extracción de device_id desde el topic
# =============================================================================

def extraer_device_id(topic: str) -> str | None:
    """
    Espera topics con formato: dispositivos/<device_id>/datos
    Retorna el device_id o None si el formato no coincide.

    Ejemplos:
        "dispositivos/esp32_01/datos"  ->  "esp32_01"
        "otro/topic"                   ->  None
    """
    partes = topic.split("/")
    if len(partes) == 3 and partes[0] == "dispositivos" and partes[2] == "datos":
        return partes[1]
    return None

# =============================================================================
# Callbacks MQTT
# =============================================================================

def on_connect(client, userdata, flags, rc, props=None):
    if rc == 0:
        log.info("Conectado al broker MQTT (%s:%s)", MQTT_HOST, MQTT_PORT)
        client.subscribe(MQTT_TOPIC)
        log.info("Suscrito a: %s", MQTT_TOPIC)
    else:
        log.error("Fallo de conexión al broker, código: %s", rc)


def on_disconnect(client, userdata, rc, props=None, reason=None):
    if rc != 0:
        log.warning("Desconexión inesperada (rc=%s). Reconectando en %ss...", rc, RECONNECT_DELAY)


def on_message(client, userdata, msg):
    topic   = msg.topic
    raw     = msg.payload.decode(errors="replace")
    con     = userdata["db"]

    # 1. Extraer device_id del topic
    device_id = extraer_device_id(topic)
    if device_id is None:
        log.warning("Topic inesperado ignorado: %s", topic)
        return

    # 2. Validar que el payload sea JSON válido
    try:
        datos = json.loads(raw)
    except json.JSONDecodeError as e:
        log.warning("Payload JSON inválido en [%s]: %s | error: %s", topic, raw, e)
        return

    # 3. Verificar que tenga al menos los campos esperados del ESP32
    _campos_raw = os.getenv("MQTT_CAMPOS_ESPERADOS", "pm25,pm10,gas,temp,hum,pres")
    campos_esperados = set(c.strip() for c in _campos_raw.split(",") if c.strip())
    if not campos_esperados.issubset(datos.keys()):
        log.warning("Payload incompleto en [%s]: faltan campos %s", topic, campos_esperados - datos.keys())
        # Se almacena igual pero se advierte — útil durante desarrollo
    
    # 4. Persistir en SQLite (se guarda el JSON original completo)
    try:
        insertar_lectura(con, device_id, topic, raw)
        log.info("[%s] device=%s | %s", topic, device_id, raw)
    except sqlite3.Error as e:
        log.error("Error al escribir en SQLite: %s", e)

# =============================================================================
# Punto de entrada
# =============================================================================

def main():
    # Inicializar DB
    con = init_db(DB_PATH)

    # Configurar cliente MQTT
    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id="data-logger-gateway",
    )
    client.user_data_set({"db": con})

    if MQTT_USER and MQTT_PASS:
        client.username_pw_set(MQTT_USER, MQTT_PASS)
    else:
        log.warning("MQTT_USER / MQTT_PASS no configurados. Conectando sin autenticación.")

    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message

    # Reconexión automática con backoff simple
    while True:
        try:
            log.info("Conectando al broker %s:%s...", MQTT_HOST, MQTT_PORT)
            client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
            client.loop_forever()
        except (ConnectionRefusedError, OSError) as e:
            log.error("No se pudo conectar: %s. Reintentando en %ss...", e, RECONNECT_DELAY)
            time.sleep(RECONNECT_DELAY)


if __name__ == "__main__":
    main()
