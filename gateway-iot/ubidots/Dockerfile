FROM python:3.12-slim
WORKDIR /app
RUN pip install paho-mqtt --no-cache-dir
COPY gateway_ubidots_v3.py .
CMD ["python", "gateway_ubidots_v3.py"]
