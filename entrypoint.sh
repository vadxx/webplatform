#!/bin/sh
set -eu

# Volume may be created empty - guarantee the layout
mkdir -p /data/work /data/tmp /data/logs

# Hint in docker logs on every start (port may change between runs)
echo "webplatform ready"
echo "  exec: docker exec -it webplatform bash   (workdir /data/work)"
echo "  http: port ${WEBPLATFORM_HTTP_PORT:-8080}   (server will listen here)"
echo "  logs: /data/logs"

# Keep-alive PID 1
exec tail -f /dev/null
