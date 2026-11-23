package config

import (
	"crypto/rand"
	"encoding/base64"
	"errors"
	"fmt"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"
)

type Config struct {
	Listen              string
	DBPath              string
	BlobRoot            string
	PublicBaseURL       string
	CDNURLs             []string
	JWTSecret           []byte
	JWTIssuer           string
	HMACKeys            map[string][]byte
	HMACKeyID           string
	GraySalt            string
	SessionTTL          time.Duration
	TokenTTL            time.Duration
	SignedURLTTL        time.Duration
	MaxUploadBytes      int64
	MaxManifestBytes    int64
	MaxBundleBytes      int64
	SecureCookies       bool
	RequireSignedFiles  bool
	TrustProxy          bool
	AdminUsername       string
	AdminPassword       string
	AllowedOrigins      []string
	RateLimitLogin      int
	RateLimitToken      int
	RateLimitUpload     int
	RateLimitDownload   int
	RateLimitDefault    int
	RateWindow          time.Duration
	EphemeralSecrets    bool
	MinAppVersion       string
	ReadHeaderTimeout   time.Duration
	ReadTimeout         time.Duration
	WriteTimeout        time.Duration
	IdleTimeout         time.Duration
	ShutdownGracePeriod time.Duration
}

const (
	DefaultListen       = ":8080"
	DefaultDBPath       = "data/parcel.db"
	DefaultBlobRoot     = "data/blobs"
	DefaultSessionTTL   = 12 * time.Hour
	DefaultTokenTTL     = time.Hour
	DefaultSignedURLTTL = 10 * time.Minute
	DefaultMaxUpload    = int64(4) << 30
	DefaultMaxManifest  = int64(64) << 20
	DefaultMaxBundle    = int64(4) << 30
	DefaultGraySalt     = "parcel"
)

func Load() (Config, error) {
	cfg := Config{
		Listen:              envString("PARCEL_LISTEN", DefaultListen),
		DBPath:              envString("PARCEL_DB", DefaultDBPath),
		BlobRoot:            envString("PARCEL_BLOB_ROOT", DefaultBlobRoot),
		PublicBaseURL:       strings.TrimRight(envString("PARCEL_PUBLIC_BASE_URL", ""), "/"),
		JWTIssuer:           envString("PARCEL_JWT_ISSUER", "parcel"),
		HMACKeyID:           envString("PARCEL_HMAC_KEY_ID", "primary"),
		GraySalt:            envString("PARCEL_GRAY_SALT", DefaultGraySalt),
		SessionTTL:          envDuration("PARCEL_SESSION_TTL", DefaultSessionTTL),
		TokenTTL:            envDuration("PARCEL_TOKEN_TTL", DefaultTokenTTL),
		SignedURLTTL:        envDuration("PARCEL_SIGNED_URL_TTL", DefaultSignedURLTTL),
		MaxUploadBytes:      envInt64("PARCEL_MAX_UPLOAD_BYTES", DefaultMaxUpload),
		MaxManifestBytes:    envInt64("PARCEL_MAX_MANIFEST_BYTES", DefaultMaxManifest),
		MaxBundleBytes:      envInt64("PARCEL_MAX_BUNDLE_BYTES", DefaultMaxBundle),
		SecureCookies:       envBool("PARCEL_SECURE_COOKIES", false),
		RequireSignedFiles:  envBool("PARCEL_REQUIRE_SIGNED_FILES", true),
		TrustProxy:          envBool("PARCEL_TRUST_PROXY", false),
		AdminUsername:       envString("PARCEL_ADMIN_USER", "admin"),
		AdminPassword:       os.Getenv("PARCEL_ADMIN_PASSWORD"),
		RateLimitLogin:      envInt("PARCEL_RATE_LOGIN", 10),
		RateLimitToken:      envInt("PARCEL_RATE_TOKEN", 60),
		RateLimitUpload:     envInt("PARCEL_RATE_UPLOAD", 20),
		RateLimitDownload:   envInt("PARCEL_RATE_DOWNLOAD", 240),
		RateLimitDefault:    envInt("PARCEL_RATE_DEFAULT", 120),
		RateWindow:          envDuration("PARCEL_RATE_WINDOW", time.Minute),
		MinAppVersion:       envString("PARCEL_MIN_APP_VERSION", ""),
		ReadHeaderTimeout:   envDuration("PARCEL_READ_HEADER_TIMEOUT", 10*time.Second),
		ReadTimeout:         envDuration("PARCEL_READ_TIMEOUT", 10*time.Minute),
		WriteTimeout:        envDuration("PARCEL_WRITE_TIMEOUT", 10*time.Minute),
		IdleTimeout:         envDuration("PARCEL_IDLE_TIMEOUT", 120*time.Second),
		ShutdownGracePeriod: envDuration("PARCEL_SHUTDOWN_GRACE", 15*time.Second),
	}
	if raw := envString("PARCEL_CDN_URLS", ""); raw != "" {
		for _, value := range strings.Split(raw, ",") {
			value = strings.TrimRight(strings.TrimSpace(value), "/")
			if value != "" {
				cfg.CDNURLs = append(cfg.CDNURLs, value)
			}
		}
	}
	if raw := envString("PARCEL_ALLOWED_ORIGINS", ""); raw != "" {
		for _, value := range strings.Split(raw, ",") {
			value = strings.TrimSpace(value)
			if value != "" {
				cfg.AllowedOrigins = append(cfg.AllowedOrigins, value)
			}
		}
	}
	if cfg.DBPath == "" || cfg.BlobRoot == "" {
		return Config{}, errors.New("PARCEL_DB and PARCEL_BLOB_ROOT must not be empty")
	}
	if cfg.MaxUploadBytes <= 0 || cfg.MaxManifestBytes <= 0 || cfg.MaxBundleBytes <= 0 {
		return Config{}, errors.New("upload size limits must be positive")
	}
	if cfg.MaxManifestBytes > cfg.MaxUploadBytes {
		cfg.MaxManifestBytes = cfg.MaxUploadBytes
	}
	if cfg.MaxBundleBytes > cfg.MaxUploadBytes {
		cfg.MaxBundleBytes = cfg.MaxUploadBytes
	}
	if cfg.PublicBaseURL == "" {
		host := cfg.Listen
		if strings.HasPrefix(host, ":") {
			host = "localhost" + host
		}
		cfg.PublicBaseURL = "http://" + host
	}
	if err := validateBaseURL(cfg.PublicBaseURL); err != nil {
		return Config{}, err
	}
	for _, value := range cfg.CDNURLs {
		if err := validateBaseURL(value); err != nil {
			return Config{}, fmt.Errorf("invalid PARCEL_CDN_URLS entry %q: %w", value, err)
		}
	}
	if cfg.AdminUsername == "" {
		cfg.AdminUsername = "admin"
	}
	if cfg.SessionTTL <= 0 || cfg.TokenTTL <= 0 || cfg.SignedURLTTL <= 0 {
		return Config{}, errors.New("token lifetimes must be positive")
	}
	if cfg.RateWindow <= 0 {
		cfg.RateWindow = time.Minute
	}

	cfg.JWTSecret = []byte(os.Getenv("PARCEL_JWT_SECRET"))
	if len(cfg.JWTSecret) == 0 {
		cfg.JWTSecret = randomSecret()
		cfg.EphemeralSecrets = true
	}
	keys, err := parseKeys(os.Getenv("PARCEL_HMAC_KEYS"))
	if err != nil {
		return Config{}, err
	}
	if len(keys) == 0 {
		keys = map[string][]byte{cfg.HMACKeyID: randomSecret()}
		cfg.EphemeralSecrets = true
	}
	cfg.HMACKeys = keys
	if len(cfg.HMACKeys[cfg.HMACKeyID]) == 0 {
		for id := range cfg.HMACKeys {
			cfg.HMACKeyID = id
			break
		}
	}
	return cfg, nil
}

func validateBaseURL(value string) error {
	parsed, err := url.Parse(value)
	if err != nil {
		return err
	}
	if parsed.Scheme != "http" && parsed.Scheme != "https" {
		return errors.New("base URL must use http or https")
	}
	if parsed.Host == "" || parsed.RawQuery != "" || parsed.Fragment != "" {
		return errors.New("base URL must contain a host without query or fragment")
	}
	return nil
}

func parseKeys(raw string) (map[string][]byte, error) {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return nil, nil
	}
	keys := make(map[string][]byte)
	for _, entry := range strings.Split(raw, ",") {
		entry = strings.TrimSpace(entry)
		if entry == "" {
			continue
		}
		id, secret, ok := strings.Cut(entry, ":")
		id = strings.TrimSpace(id)
		if !ok || id == "" || secret == "" {
			return nil, errors.New("invalid PARCEL_HMAC_KEYS entry (want id:secret)")
		}
		if _, exists := keys[id]; exists {
			return nil, fmt.Errorf("duplicate HMAC key id %q", id)
		}
		keys[id] = []byte(secret)
	}
	return keys, nil
}

func randomSecret() []byte {
	value := make([]byte, 32)
	if _, err := rand.Read(value); err != nil {
		return []byte(base64.RawURLEncoding.EncodeToString([]byte(time.Now().String())))
	}
	return []byte(base64.RawURLEncoding.EncodeToString(value))
}

func envString(key, fallback string) string {
	if value := strings.TrimSpace(os.Getenv(key)); value != "" {
		return value
	}
	return fallback
}

func envBool(key string, fallback bool) bool {
	value := strings.TrimSpace(os.Getenv(key))
	if value == "" {
		return fallback
	}
	parsed, err := strconv.ParseBool(value)
	if err != nil {
		return fallback
	}
	return parsed
}

func envInt(key string, fallback int) int {
	value := strings.TrimSpace(os.Getenv(key))
	if value == "" {
		return fallback
	}
	parsed, err := strconv.Atoi(value)
	if err != nil {
		return fallback
	}
	return parsed
}

func envInt64(key string, fallback int64) int64 {
	value := strings.TrimSpace(os.Getenv(key))
	if value == "" {
		return fallback
	}
	parsed, err := strconv.ParseInt(value, 10, 64)
	if err != nil {
		return fallback
	}
	return parsed
}

func envDuration(key string, fallback time.Duration) time.Duration {
	value := strings.TrimSpace(os.Getenv(key))
	if value == "" {
		return fallback
	}
	parsed, err := time.ParseDuration(value)
	if err != nil {
		return fallback
	}
	return parsed
}
