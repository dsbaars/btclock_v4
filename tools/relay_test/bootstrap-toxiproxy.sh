#!/usr/bin/env bash
# Register the three relay-upstream proxies with the running toxiproxy
# container. Idempotent — safe to re-run after `docker compose up`.
#
# Toxiproxy's REST API is at :8474. We use POST /populate to register
# all three proxies in one shot; the call is a no-op if they already
# exist with the same listen/upstream config.
set -euo pipefail

ADMIN="${ADMIN:-http://127.0.0.1:8474}"

# Wait for the admin API to be reachable. compose's healthcheck does
# this for the depends_on link, but a freshly-started container needs a
# moment when invoking this script standalone.
for _ in $(seq 1 30); do
  if curl -sf "${ADMIN}/version" >/dev/null 2>&1; then
    break
  fi
  sleep 1
done
if ! curl -sf "${ADMIN}/version" >/dev/null 2>&1; then
  echo "toxiproxy admin API not reachable at ${ADMIN}" >&2
  exit 1
fi

# `listen` uses 0.0.0.0 so sniproxy (in another container) can reach
# the listeners via the docker bridge AND the host can poke them
# directly for smoke tests.
curl -s -X POST -H 'Content-Type: application/json' \
     -d '[
           {"name":"primal", "listen":"0.0.0.0:9443",
            "upstream":"relay.primal.net:443", "enabled":true},
           {"name":"noslol", "listen":"0.0.0.0:9444",
            "upstream":"nos.lol:443", "enabled":true},
           {"name":"dbtc",   "listen":"0.0.0.0:9445",
            "upstream":"nostr.dbtc.link:443", "enabled":true}
         ]' \
     "${ADMIN}/populate" | jq .
echo "registered: primal noslol dbtc"
