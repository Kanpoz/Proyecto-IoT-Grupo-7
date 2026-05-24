FROM python:3.12-slim
WORKDIR /app
RUN pip install paho-mqtt google-genai --no-cache-dir
COPY ia_analisisV2.py .
CMD ["python", "ia_analisisV2.py"]
