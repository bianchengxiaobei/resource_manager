FROM node:24-alpine AS web
WORKDIR /src/web
COPY web/package.json web/package-lock.json ./
RUN npm ci
COPY web/ ./
RUN npm run build

FROM golang:1.27.4-bookworm AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
COPY --from=web /src/internal/webui/dist ./internal/webui/dist
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/parcel-server ./cmd/parcel-server
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/parcel-admin ./cmd/parcel-admin

FROM gcr.io/distroless/static-debian12
WORKDIR /data
COPY --from=build /out/parcel-server /parcel-server
COPY --from=build /out/parcel-admin /parcel-admin
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 CMD ["/parcel-server", "healthcheck"]
ENTRYPOINT ["/parcel-server"]

