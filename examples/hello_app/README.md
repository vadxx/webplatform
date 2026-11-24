# examples/app

The app pattern built on the server slice: everything interesting sits
behind a login gate. The dashboard page needs a live session cookie,
API endpoints answer `401` instead of redirecting, and sessions are
stored in SQLite so they survive process restarts.

## What it demonstrates

- Session gate in one handler: pages redirect to `/login` (302), API
  endpoints under `/api/` answer `401` so `fetch` callers do not chase
  a redirect they cannot follow
- `POST /login` checks the password against `HELLO_APP_PASSWORD`
  (default `admin` - see the warning below), issues an `app_session`
  cookie backed by `core_session_*` tokens and writes it through to
  SQLite; `POST /logout` deletes the row and the cookie
- Sessions hydrate from the database on start - kill the process and
  the browser (or a saved cookie jar) is still logged in
- `/api/logs` - a ring buffer of the server's own log lines served as
  JSON behind the gate (best effort: under memory pressure it answers
  `503 busy`)
- The same metrics pipeline as `examples/server`: `/api/metrics` from
  a timerfd collector into SQLite, drawn by a uPlot chart
- Public surface is minimal: the login page and the assets it needs;
  everything else requires the cookie

## Run (from the host)

Prerequisite: the dev container is up (`docker compose up -d`), the
repo is synced and `./scripts/check.sh` has built the binary. While
the app runs, the login page opens in a host browser at
`http://localhost:8080`.

```bash
docker exec -d webplatform sh -c 'cd /data/work/webplatform && build/examples/hello_app/src/hello_app >>/data/logs/app.log 2>&1'
docker exec webplatform sh -c 'curl -si localhost:8080/ | head -3'         # -> 302 Found, Location: /login
docker exec webplatform sh -c 'curl -si localhost:8080/api/logs | head -1' # -> HTTP/1.1 401 Unauthorized
docker exec webplatform curl -sf -c /tmp/jar -d password=admin localhost:8080/login -o /dev/null
docker exec webplatform curl -sf --compressed -b /tmp/jar localhost:8080/ | grep log-lines
docker exec webplatform curl -sf -b /tmp/jar localhost:8080/api/logs | grep '"lines"'
# persistence: restart keeps the session
docker exec webplatform pkill hello_app
docker exec -d webplatform sh -c 'cd /data/work/webplatform && build/examples/hello_app/src/hello_app >>/data/logs/app.log 2>&1'
docker exec webplatform curl -sf --compressed -b /tmp/jar localhost:8080/ | grep log-lines
docker exec webplatform pkill hello_app                                    # stop
```

## Environment

- `HELLO_APP_PASSWORD` - login password, default `admin`. This is a
  demo: the default ships in plain text in a public repository and is
  known to everyone. Set your own value for anything beyond a local
  demo, and treat the login as a pattern reference, not a security
  boundary. There is no URL decoding, so browsers (which always
  percent-encode) only work with passwords made of unreserved
  characters: letters, digits and `- . _ ~`. An explicitly empty value
  refuses to start the app; unset falls back to `admin` with a warning
  in the log.
- `WEBPLATFORM_DB` - SQLite file for sessions and metrics, default
  `/data/metrics.db` (the same file `examples/server` uses; both
  create their tables with `IF NOT EXISTS`).
- `WEBPLATFORM_HTTP_PORT` - listen port, default `8080`.
