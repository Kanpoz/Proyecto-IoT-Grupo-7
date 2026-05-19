import os, time, sqlite3, json
import paho.mqtt.client as mqtt

MQTT_HOST = os.getenv("MQTT_HOST", "mosquitto")
MQTT_USER = os.getenv("MQTT_USER")
MQTT_PASS = os.getenv("MQTT_PASS")
DB_PATH   = os.getenv("DB_PATH", "/data/sensores.db")

def init_db():
    con = sqlite3.connect(DB_PATH)
    con.execute("""
        CREATE TABLE IF NOT EXISTS lecturas (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            topic     TEXT,
            payload   TEXT,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    """)
    con.commit()
    return con

def on_connect(client, userdata, flags, rc, props=None):
    print(f"Conectado al broker, código: {rc}")
    client.subscribe("#")  # suscribe a todos los topics

def on_message(client, userdata, msg):
    payload = msg.payload.decode()
    print(f"[{msg.topic}] {payload}")
    userdata.execute(
        "INSERT INTO lecturas (topic, payload) VALUES (?, ?)",
        (msg.topic, payload)
    )
    userdata.commit()

con = init_db()
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.user_data_set(con)
client.username_pw_set(MQTT_USER, MQTT_PASS)
client.on_connect = on_connect
client.on_message = on_message

print("Conectando al broker...")
client.connect(MQTT_HOST, 1883, 60)
client.loop_forever()
