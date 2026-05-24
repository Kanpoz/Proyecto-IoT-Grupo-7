import paho.mqtt.client as mqtt
import json
import sqlite3
import time
import os
import logging

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger(__name__)

# ── Configuración Ubidots ──────────────────────────────────────────────────────
TOKEN        = os.getenv("UBIDOTS_TOKEN")
DEVICE_LABEL = os.getenv("DEVICE_LABEL")
DB_PATH      = os.getenv("DB_PATH", "/data/sensores.db")

UBIDOTS_BROKER = "industrial.api.ubidots.com"
POLL_INTERVAL  = 2

# ── Configuración Mosquitto (broker local) ─────────────────────────────────────
MQTT_LOCAL_HOST = os.getenv("MQTT_HOST", "mosquitto")
MQTT_LOCAL_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_LOCAL_USER = os.getenv("MQTT_USER")
MQTT_LOCAL_PASS = os.getenv("MQTT_PASS")

# Topic en Ubidots desde el que llegan los comandos (Last Value / LV API)
# Ubidots publica en /v1.6/devices/{device}/{variable}/lv
UBIDOTS_CMD_TOPIC = f"/v1.6/devices/{DEVICE_LABEL}/comandos/lv"

# Topic local al que el ESP32 está suscrito
LOCAL_CMD_TOPIC = "dispositivos/esp32_01/comandos"


# ── Persiste el último ID enviado en la propia DB ─────────────────────────────
def obtener_ultimo_id(conn):
    conn.execute("""
        CREATE TABLE IF NOT EXISTS gateway_state (
            clave TEXT PRIMARY KEY,
            valor INTEGER
        )
    """)
    conn.commit()
    row = conn.execute(
        "SELECT valor FROM gateway_state WHERE clave='ultimo_id'"
    ).fetchone()
    return row[0] if row else 0


def guardar_ultimo_id(conn, id_reg):
    conn.execute("""
        INSERT INTO gateway_state (clave, valor) VALUES ('ultimo_id', ?)
        ON CONFLICT(clave) DO UPDATE SET valor=excluded.valor
    """, (id_reg,))
    conn.commit()


# ── Cliente Mosquitto local (para reenviar comandos al ESP32) ─────────────────
local_client = mqtt.Client(
    callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
    client_id="gateway-cmd-bridge",
)


def conectar_mosquitto():
    if MQTT_LOCAL_USER:
        local_client.username_pw_set(MQTT_LOCAL_USER, MQTT_LOCAL_PASS)
    local_client.reconnect_delay_set(min_delay=2, max_delay=30)

    def on_connect_local(client, userdata, flags, rc, props=None):
        if rc == 0:
            log.info("Conectado a Mosquitto local")
        else:
            log.error("Fallo conexión Mosquitto local, rc=%s", rc)

    def on_disconnect_local(client, userdata, rc, props=None, reason=None):
        if rc != 0:
            log.warning("Desconectado de Mosquitto local (rc=%s). Reconectando...", rc)

    local_client.on_connect    = on_connect_local
    local_client.on_disconnect = on_disconnect_local
    local_client.connect(MQTT_LOCAL_HOST, MQTT_LOCAL_PORT, keepalive=60)
    local_client.loop_start()


# ── Callbacks cliente Ubidots ─────────────────────────────────────────────────
def on_connect_ubidots(client, userdata, flags, rc, props=None):
    if rc == 0:
        log.info("Conectado a Ubidots MQTT")
        # Suscribirse al topic de comandos de Ubidots
        client.subscribe(UBIDOTS_CMD_TOPIC, qos=1)
        log.info("Suscrito a comandos Ubidots: %s", UBIDOTS_CMD_TOPIC)
    else:
        log.error("Fallo conexión Ubidots, rc=%s", rc)


def on_disconnect_ubidots(client, userdata, rc, props=None, reason=None):
    if rc != 0:
        log.warning("Desconectado de Ubidots (rc=%s). Reconectando...", rc)


def on_message_ubidots(client, userdata, msg):
    try:
        payload_str = msg.payload.decode("utf-8").strip()
        log.info("Comando recibido desde Ubidots [%s]: %s", msg.topic, payload_str)

        try:
            value = float(payload_str)
            data = {"value": value}
        except ValueError:
            data = json.loads(payload_str)

        if "cmd" in data:
            cmd_payload = json.dumps({"cmd": data["cmd"]})
        elif data.get("value") == 1.0:
            cmd_payload = json.dumps({"cmd": "alarm_off"})
        else:
            log.warning("Payload de comando no reconocido: %s", payload_str)
            return

        result = local_client.publish(LOCAL_CMD_TOPIC, cmd_payload, qos=1)
        result.wait_for_publish(timeout=5)
        log.info("Comando reenviado al ESP32 [%s]: %s", LOCAL_CMD_TOPIC, cmd_payload)

    except Exception as e:
        log.error("Error procesando comando de Ubidots: %s", e)


# ── Gateway principal ──────────────────────────────────────────────────────────
def ejecutar_gateway():
    # Conexión a Mosquitto local (para el puente de comandos)
    conectar_mosquitto()

    # Conexión a Ubidots
    ubidots_client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id="gateway-ubidots",
    )
    ubidots_client.username_pw_set(TOKEN, "")
    ubidots_client.on_connect    = on_connect_ubidots
    ubidots_client.on_disconnect = on_disconnect_ubidots
    ubidots_client.on_message    = on_message_ubidots
    ubidots_client.reconnect_delay_set(min_delay=2, max_delay=30)
    ubidots_client.connect(UBIDOTS_BROKER, 1883, keepalive=60)
    ubidots_client.loop_start()

    conn      = sqlite3.connect(DB_PATH)
    ultimo_id = obtener_ultimo_id(conn)
    log.info("Gateway activo. Reanudando desde ID %s", ultimo_id)

    try:
        while True:
            cursor = conn.cursor()
            cursor.execute(
                "SELECT id, device_id, payload FROM lecturas "
                "WHERE id > ? ORDER BY id ASC LIMIT 10",
                (ultimo_id,)
            )
            filas = cursor.fetchall()

            for id_reg, device_id, payload_raw in filas:
                try:
                    datos  = json.loads(payload_raw)
                    topic  = f"/v1.6/devices/{device_id}"
                    result = ubidots_client.publish(topic, json.dumps(datos), qos=1)
                    result.wait_for_publish(timeout=5)
                    ultimo_id = id_reg
                    guardar_ultimo_id(conn, ultimo_id)
                    log.info("✓ ID %s → %s", id_reg, topic)
                except Exception as e:
                    log.error("Error enviando ID %s: %s", id_reg, e)
                    break  # reintenta en el próximo ciclo

            time.sleep(POLL_INTERVAL)

    except KeyboardInterrupt:
        log.info("Gateway detenido.")
    finally:
        ubidots_client.loop_stop()
        ubidots_client.disconnect()
        local_client.loop_stop()
        local_client.disconnect()
        conn.close()


if __name__ == "__main__":
    ejecutar_gateway()