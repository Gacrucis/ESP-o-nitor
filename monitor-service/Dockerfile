FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1
ENV PYTHONPATH=/app/src

WORKDIR /app

# watchdog/watchmedo: reinicio en caliente. pillow: renderizado de los frames OLED.
RUN pip install --no-cache-dir watchdog==6.0.0 pillow==11.0.0

COPY src ./src

EXPOSE 8765

# Hot reloading: reinicia el servidor cuando cambia cualquier *.py montado.
CMD ["watchmedo", "auto-restart", "--directory=/app/src", "--pattern=*.py", "--recursive", "--", "python", "-m", "monitor_service.main"]
