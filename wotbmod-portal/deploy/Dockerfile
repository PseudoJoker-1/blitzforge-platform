# docker build -f deploy/Dockerfile -t wotbmod-portal .
# docker run -p 8080:8080 -v wotbmod-data:/app/data -v $PWD/config.json:/app/config.json:ro wotbmod-portal
FROM python:3.12-slim
WORKDIR /app
COPY backend /app/backend
COPY frontend /app/frontend
COPY tools /app/tools
COPY config.example.json /app/config.example.json
RUN useradd --system --home /app wotbmod && mkdir -p /app/data && chown -R wotbmod /app/data
USER wotbmod
EXPOSE 8080
CMD ["python", "backend/portal_server.py", "serve", "--host", "0.0.0.0", "--quiet"]
