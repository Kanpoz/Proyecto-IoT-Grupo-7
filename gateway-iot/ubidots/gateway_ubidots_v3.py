import paho.mqtt.client as mqtt
import json
import sqlite3
import time
import os
import logging

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger(__name__)

TOKEN        = os.getenv("UBIDOTS_TOKEN")
DEVICE_LABEL = os.getenv("DEVICE_LABEL")
DB_PATH      = os.getenv("DB_PATH", "/data/sensores.db")
MQTT_BROKER  = "industrial.api.ubidots.com"
POLL_INTERVAL = 2

# ── Persiste el último ID enviado en la propia DB ──────────────────────────
def obtener_ultimo_id(conn):
    conn.execute("""
        CREATE TABLE IF NOT EXISTS gateway_state (
            clave TEXT PRIMARY KEY,
            valor INTEGER
        )
    """)
    conn.commit()
    row = conn.execute("SELECT valor FROM gateway_state WHERE clave='ultimo_id'").fetchone()
    return row[0] if row else 0

def guardar_ultimo_id(conn, id_reg):
    conn.execute("""
        INSERT INTO gateway_state (clave, valor) VALUES ('ultimo_id', ?)
        ON CONFLICT(clave) DO UPDATE SET valor=excluded.valor
    """, (id_reg,))
    conn.commit()

# ── Callbacks MQTT ─────────────────────────────────────────────────────────
def on_connect(client, userdata, flags, rc, props=None):
    if rc == 0:
        log.info("Conectado a Ubidots MQTT")
    else:
        log.error("Fallo conexión Ubidots, rc=%s", rc)

def on_disconnect(client, userdata, rc, props=None, reason=None):
    if rc != 0:
        log.warning("Desconectado de Ubidots (rc=%s). Reconectando...", rc)

# ── Gateway principal ──────────────────────────────────────────────────────
def ejecutar_gateway():
    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id="gateway-ubidots",
    )
    client.username_pw_set(TOKEN, "")
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect

    client.reconnect_delay_set(min_delay=2, max_delay=30)  # backoff automático
    client.connect(MQTT_BROKER, 1883, keepalive=60)
    client.loop_start()  # ← mantiene keepalive en segundo plano

    conn = sqlite3.connect(DB_PATH)
    ultimo_id = obtener_ultimo_id(conn)  # ← sobrevive reinicios
    log.info("Gateway activo. Reanudando desde ID %s", ultimo_id)

    try:
        while True:
            cursor = conn.cursor()
            cursor.execute(
                "SELECT id, device_id, payload FROM lecturas WHERE id > ? ORDER BY id ASC LIMIT 10",
                (ultimo_id,)
            )
            filas = cursor.fetchall()

            for id_reg, device_id, payload_raw in filas:
                try:
                    datos = json.loads(payload_raw)
                    topic = f"/v1.6/devices/{device_id}"  # ← usa el device_id real
                    result = client.publish(topic, json.dumps(datos), qos=1)
                    result.wait_for_publish(timeout=5)    # ← confirma entrega
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
        client.loop_stop()
        client.disconnect()
        conn.close()

if __name__ == "__main__":
    ejecutar_gateway()