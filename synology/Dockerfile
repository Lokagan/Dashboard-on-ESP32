FROM python:3.12-slim
WORKDIR /app
RUN pip install --no-cache-dir \
    paho-mqtt==2.1.0 \
    requests==2.34.2 \
    flask==3.1.3 \
    openai==2.44.0 \
    edge-tts==7.2.8 \
    pydub==0.25.1 \
    static-ffmpeg==2.13 \
    ddgs==9.14.4
CMD ["python3", "-u", "monitor.py"]