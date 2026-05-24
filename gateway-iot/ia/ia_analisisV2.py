import paho.mqtt.client as mqtt
import json
import sqlite3
import time
import socket
from google import genai
import os

# --- CONFIGURACIÓN ---
TOKEN        = os.getenv("UBIDOTS_TOKEN")
DEVICE_LABEL = os.getenv("DEVICE_LABEL")
DB_PATH      = os.getenv("DB_PATH", "/data/sensores.db")
GEMINI_KEY   = os.getenv("GEMINI_API_KEY")
client_ia    = genai.Client(api_key=GEMINI_KEY)

def on_message(client, userdata, msg):
    # El valor llega como "1.0" desde el widget de Ubidots [5, 6]
    if msg.payload.decode() == "1.0":
        print("Solicitud de IA recibida. Procesando tendencia desde sensores.db...")
        ejecutar_analisis_y_publicar(client)

def ejecutar_analisis_y_publicar(mqtt_client):
    try:
        # 1. Leer los últimos 20 registros de la columna 'payload' [4, 7]
        conn = sqlite3.connect(DB_PATH)
        cursor = conn.cursor()
        cursor.execute('SELECT payload FROM lecturas ORDER BY id DESC LIMIT 20')
        filas = cursor.fetchall()
        conn.close()

        if filas:
            historial_puntos = []
            for (payload,) in filas:
                # Decodificamos el JSON guardado por el script main [3]
                d = json.loads(payload)
                historial_puntos.append(
                    f"PM2.5: {d.get('pm25')}, NO2: {d.get('no2')}, Temp: {d.get('temp')}°C"
                )
            
            # 2. Consultar a Gemini (IA recomendada) [2, 8]
            prompt = (
                f"Actúa como experto en salud ambiental en Sabana Centro. Analiza esta tendencia "
                f"de 20 registros (del más reciente al más antiguo) y determina si hay riesgo de smog "
                f"o inversión térmica. Da una recomendación breve a las autoridades, sin usar markdown:\n"
                + "\n".join(historial_puntos)
            )
            
            response = client_ia.models.generate_content(model="gemini-2.5-flash", contents=prompt)
            recomendacion = response.text

            # 3. Publicar el texto en el 'contexto' hacia Ubidots [9-11]
            # Usamos el tópico del dispositivo para enviar valores y contexto simultáneamente
            topic_pub = f"/v1.6/devices/{DEVICE_LABEL}"
            payload_ubidots = {
                "recomendacion-ia": {
                    "value": 1, 
                    "context": {"text": recomendacion}
                },
                "solicitud-ia": 0  # Reseteamos el botón en el dashboard [12]
            }
            
            mqtt_client.publish(topic_pub, json.dumps(payload_ubidots))
            print(f"✓ Análisis enviado a Ubidots: {recomendacion[:50]}...")
        else:
            print("No hay datos suficientes en la base de datos para el análisis.")

    except Exception as e:
        print(f"Error en el proceso de IA: {e}")

# --- CONFIGURACIÓN DEL CLIENTE MQTT ---
client = mqtt.Client()
# Autenticación requerida: Token como usuario y contraseña [13-15]
client.username_pw_set(TOKEN, TOKEN) 
client.on_message = on_message

print("Módulo IA V2 conectado y esperando solicitudes desde Ubidots...")
# Intento de conexión con diagnóstico DNS y manejo de errores
host_mqtt = "industrial.api.ubidots.com"
try:
    try:
        ip = socket.gethostbyname(host_mqtt)
        print(f"Resolución DNS: {host_mqtt} -> {ip}")
    except Exception as e:
        print(f"Advertencia: no se pudo resolver {host_mqtt}: {e}")

    client.connect(host_mqtt, 1883, keepalive=60)
    # Nos suscribimos al último valor (lv) de la variable de solicitud [5, 16]
    client.subscribe(f"/v1.6/devices/{DEVICE_LABEL}/solicitud-ia/lv")
    client.loop_forever()
except socket.gaierror as e:
    print(f"Error DNS (getaddrinfo) al conectar a {host_mqtt}: {e}. Verifica conectividad y DNS.")
    raise
except Exception as e:
    print(f"Error conectando al broker MQTT ({host_mqtt}): {e}")
    raise