#!/bin/sh
set -eu
if [ "$#" -ne 1 ]; then
  echo "usage: $0 backups/<timestamp>" >&2
  exit 2
fi
docker compose stop parcel-server
docker compose run --rm --entrypoint /parcel-admin parcel-server restore \
  --backup "/data/$1" --db /data/parcel.db --blobs /data/blobs
docker compose start parcel-server
