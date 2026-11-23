package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"parcel/server/internal/api"
	"parcel/server/internal/blob"
	"parcel/server/internal/config"
	"parcel/server/internal/secure"
	"parcel/server/internal/store"
	"parcel/server/internal/webui"
)

var version = "dev"

func main() {
	if len(os.Args) > 1 && os.Args[1] == "healthcheck" {
		os.Exit(healthcheck())
	}
	cfg, err := config.Load()
	if err != nil {
		fmt.Fprintln(os.Stderr, "configuration error:", err)
		os.Exit(2)
	}
	logger := newLogger()
	if cfg.EphemeralSecrets {
		logger.Warn("ephemeral JWT/HMAC secrets are in use; configure PARCEL_JWT_SECRET and PARCEL_HMAC_KEYS in production")
	}
	st, err := store.Open(cfg.DBPath)
	if err != nil {
		logger.Error("open store failed", "error", err)
		os.Exit(1)
	}
	defer st.Close()
	blobs, err := blob.New(cfg.BlobRoot)
	if err != nil {
		logger.Error("open blob store failed", "error", err)
		os.Exit(1)
	}
	if err := blobs.CleanTemp(); err != nil {
		logger.Warn("could not clean temporary uploads", "error", err)
	}
	if cfg.AdminPassword != "" {
		hash, err := secure.HashPassword(cfg.AdminPassword)
		if err != nil {
			logger.Error("hash bootstrap password failed", "error", err)
			os.Exit(1)
		}
		created, err := st.BootstrapAdmin(context.Background(), cfg.AdminUsername, hash)
		if err != nil {
			logger.Error("bootstrap admin failed", "error", err)
			os.Exit(1)
		}
		if created {
			logger.Info("bootstrap admin created", "username", cfg.AdminUsername)
		}
	} else {
		logger.Warn("PARCEL_ADMIN_PASSWORD is unset; first login is unavailable until an admin is bootstrapped")
	}
	server, err := api.New(cfg, st, blobs, logger, webui.FS(), version)
	if err != nil {
		logger.Error("create API server failed", "error", err)
		os.Exit(1)
	}
	httpServer := &http.Server{
		Addr:              cfg.Listen,
		Handler:           server.Handler(),
		ReadHeaderTimeout: cfg.ReadHeaderTimeout,
		ReadTimeout:       cfg.ReadTimeout,
		WriteTimeout:      cfg.WriteTimeout,
		IdleTimeout:       cfg.IdleTimeout,
	}
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-stop
		ctx, cancel := context.WithTimeout(context.Background(), cfg.ShutdownGracePeriod)
		defer cancel()
		if err := httpServer.Shutdown(ctx); err != nil {
			logger.Error("graceful shutdown failed", "error", err)
		}
	}()
	logger.Info("parcel server listening", "address", cfg.Listen, "public_base_url", cfg.PublicBaseURL,
		"version", version)
	if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		logger.Error("http server failed", "error", err)
		os.Exit(1)
	}
}

func newLogger() *slog.Logger {
	level := slog.LevelInfo
	switch strings.ToLower(strings.TrimSpace(os.Getenv("PARCEL_LOG_LEVEL"))) {
	case "debug":
		level = slog.LevelDebug
	case "warn":
		level = slog.LevelWarn
	case "error":
		level = slog.LevelError
	}
	options := &slog.HandlerOptions{Level: level}
	if strings.EqualFold(os.Getenv("PARCEL_LOG_FORMAT"), "text") {
		return slog.New(slog.NewTextHandler(os.Stdout, options))
	}
	return slog.New(slog.NewJSONHandler(os.Stdout, options))
}

func healthcheck() int {
	url := strings.TrimRight(os.Getenv("PARCEL_HEALTH_URL"), "/")
	if url == "" {
		listen := os.Getenv("PARCEL_LISTEN")
		if listen == "" {
			listen = config.DefaultListen
		}
		if strings.HasPrefix(listen, ":") {
			listen = "127.0.0.1" + listen
		}
		url = "http://" + listen
	}
	client := http.Client{Timeout: 3 * time.Second}
	response, err := client.Get(url + "/healthz")
	if err != nil {
		return 1
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return 1
	}
	return 0
}
