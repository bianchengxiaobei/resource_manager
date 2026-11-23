# Parcel deployment

1. Copy `.env.example` to `.env` and replace every secret.
2. Provide TLS certificates at `certs/server.crt` and `certs/server.key`
   (self-signed is fine for local smoke tests).
3. Run `docker compose --env-file .env up -d --build`.
4. Open `https://localhost/` for the admin UI and `https://localhost/healthz`.
5. Backups: `sh backup.sh` (writes `/data/backups/<timestamp>/`).
6. Restore: `sh restore.sh backups/<timestamp>` after verifying the backup.

The default compose file enforces HTTPS, secure cookies and signed file URLs.
The server data directory is the named volume `parcel-data`; SQLite and
content-addressed blobs live there. A backup contains a consistent SQLite
snapshot (`VACUUM INTO`) plus immutable blobs.

Local development without Docker:

```sh
cd server
PARCEL_ADMIN_PASSWORD=password go run ./cmd/parcel-server
cd web && npm run dev
```

