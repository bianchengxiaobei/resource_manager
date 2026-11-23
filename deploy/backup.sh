#!/bin/sh
set -eu
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="/data/backups/${STAMP}"
docker compose exec -T parcel-server /parcel-admin backup \
  --db /data/parcel.db --blobs /data/blobs --out "${OUT}"
echo "backup created: ${OUT}"
