FROM python:3.12-slim
WORKDIR /app
RUN pip install paho-mqtt --no-cache-dir
COPY main.py .
CMD ["python", "main.py"]
