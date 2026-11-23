package api

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"net"
	"net/http"
	"runtime/debug"
	"strings"
	"sync"
	"time"

	"parcel/server/internal/blob"
	"parcel/server/internal/config"
	"parcel/server/internal/metrics"
	"parcel/server/internal/secure"
	"parcel/server/internal/store"
)

type Server struct {
	cfg     config.Config
	store   *store.Store
	blobs   *blob.Store
	logger  *slog.Logger
	metrics *metrics.Registry
	signer  *secure.Signer
	mux     *http.ServeMux
	web     fs.FS
	version string
}

type contextKey string

const (
	contextRequestID contextKey = "request_id"
	contextAdmin     contextKey = "admin"
	contextClaims    contextKey = "claims"
)

type adminIdentity struct {
	User      store.User
	CSRF      string
	APIKey    bool
	SessionID string
}

type errorBody struct {
	Code      string `json:"code"`
	Message   string `json:"message"`
	RequestID string `json:"request_id,omitempty"`
}

type pagination struct {
	Items    any `json:"items"`
	Total    int `json:"total"`
	Page     int `json:"page"`
	PageSize int `json:"page_size"`
}

func New(cfg config.Config, st *store.Store, blobs *blob.Store, logger *slog.Logger,
	web fs.FS, version string) (*Server, error) {
	signer, err := secure.NewSigner(cfg.HMACKeys, cfg.HMACKeyID, cfg.SignedURLTTL)
	if err != nil {
		return nil, err
	}
	if logger == nil {
		logger = slog.Default()
	}
	server := &Server{
		cfg:     cfg,
		store:   st,
		blobs:   blobs,
		logger:  logger,
		metrics: metrics.New(),
		signer:  signer,
		mux:     http.NewServeMux(),
		web:     web,
		version: version,
	}
	server.routes()
	return server, nil
}

func (s *Server) Handler() http.Handler {
	var handler http.Handler = s.mux
	handler = s.logging(handler)
	handler = s.recover(handler)
	handler = s.rejectTraversal(handler)
	handler = s.cors(handler)
	handler = s.security(handler)
	handler = s.requestID(handler)
	return handler
}

func (s *Server) Metrics() *metrics.Registry { return s.metrics }

func (s *Server) routes() {
	s.mux.HandleFunc("GET /healthz", s.handleHealth)
	s.mux.Handle("GET /metrics", s.metrics)
	s.mux.HandleFunc("POST /api/v1/auth/device-token", s.limit("device-token", s.cfg.RateLimitToken, s.handleDeviceToken))
	s.mux.HandleFunc("GET /api/v1/client/config", s.limit("client-config", s.cfg.RateLimitDefault, s.handleClientConfig))
	s.mux.HandleFunc("GET /api/v1/client/version", s.limit("client-version", s.cfg.RateLimitDefault, s.handleClientVersion))
	s.mux.Handle("GET /api/v1/client/url", s.limit("client-url", s.cfg.RateLimitDefault, s.requireClient(s.handleClientURL)))
	s.mux.HandleFunc("GET /files/", s.limit("files", s.cfg.RateLimitDownload, s.handleFile))

	s.mux.HandleFunc("POST /api/v1/admin/login", s.limit("login", s.cfg.RateLimitLogin, s.handleLogin))
	s.mux.HandleFunc("POST /api/v1/admin/logout", s.requireAdmin(s.handleLogout))
	s.mux.Handle("GET /api/v1/admin/me", s.requireAdmin(s.handleMe))
	s.mux.Handle("POST /api/v1/admin/api-keys", s.requireAdmin(s.handleCreateAPIKey))
	s.mux.Handle("GET /api/v1/admin/overview", s.requireAdmin(s.handleOverview))
	s.mux.Handle("GET /api/v1/admin/metrics", s.requireAdmin(s.handleMetrics))
	s.mux.Handle("GET /api/v1/admin/audit", s.requireAdmin(s.handleAudit))

	s.mux.Handle("GET /api/v1/admin/packages", s.requireAdmin(s.handleListPackages))
	s.mux.Handle("POST /api/v1/admin/packages", s.requireAdmin(s.handleCreatePackage))
	s.mux.Handle("GET /api/v1/admin/packages/{pkg}", s.requireAdmin(s.handleGetPackage))
	s.mux.Handle("GET /api/v1/admin/packages/{pkg}/versions", s.requireAdmin(s.handleListVersions))
	s.mux.Handle("POST /api/v1/admin/packages/{pkg}/versions", s.limit("upload", s.cfg.RateLimitUpload,
		s.requireAdmin(s.handleUploadVersion)))
	s.mux.Handle("GET /api/v1/admin/packages/{pkg}/versions/{version}", s.requireAdmin(s.handleGetVersion))
	s.mux.Handle("POST /api/v1/admin/packages/{pkg}/versions/{version}/publish", s.requireAdmin(s.handlePublish))
	s.mux.Handle("POST /api/v1/admin/packages/{pkg}/rollback", s.requireAdmin(s.handleRollback))
	s.mux.Handle("GET /api/v1/admin/packages/{pkg}/channels", s.requireAdmin(s.handleListChannels))
	s.mux.Handle("PUT /api/v1/admin/packages/{pkg}/channels/{channel}", s.requireAdmin(s.handleSetChannel))
	s.mux.Handle("GET /api/v1/admin/packages/{pkg}/gray-rules", s.requireAdmin(s.handleListGrayRules))
	s.mux.Handle("POST /api/v1/admin/packages/{pkg}/gray-rules", s.requireAdmin(s.handleCreateGrayRule))
	s.mux.Handle("PUT /api/v1/admin/packages/{pkg}/gray-rules/{id}", s.requireAdmin(s.handleUpdateGrayRule))
	s.mux.Handle("DELETE /api/v1/admin/packages/{pkg}/gray-rules/{id}", s.requireAdmin(s.handleDeleteGrayRule))

	s.mux.Handle("/", s.handleStatic())
}

func (s *Server) requestID(next http.Handler) http.Handler {
	return http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		id := strings.TrimSpace(request.Header.Get("X-Request-ID"))
		if id == "" || len(id) > 128 || strings.ContainsAny(id, "\r\n") {
			id, _ = secure.RandomToken(12)
		}
		writer.Header().Set("X-Request-ID", id)
		next.ServeHTTP(writer, request.WithContext(context.WithValue(request.Context(), contextRequestID, id)))
	})
}

func (s *Server) security(next http.Handler) http.Handler {
	return http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		writer.Header().Set("X-Content-Type-Options", "nosniff")
		writer.Header().Set("X-Frame-Options", "DENY")
		writer.Header().Set("Referrer-Policy", "no-referrer")
		next.ServeHTTP(writer, request)
	})
}

func (s *Server) cors(next http.Handler) http.Handler {
	return http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		origin := request.Header.Get("Origin")
		if origin != "" && s.originAllowed(origin) {
			writer.Header().Set("Access-Control-Allow-Origin", origin)
			writer.Header().Set("Vary", "Origin")
			writer.Header().Set("Access-Control-Allow-Credentials", "true")
			writer.Header().Set("Access-Control-Allow-Headers", "Authorization, Content-Type, X-CSRF-Token, X-API-Key, X-Request-ID")
			writer.Header().Set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
		}
		if request.Method == http.MethodOptions {
			writer.WriteHeader(http.StatusNoContent)
			return
		}
		next.ServeHTTP(writer, request)
	})
}

func (s *Server) originAllowed(origin string) bool {
	for _, value := range s.cfg.AllowedOrigins {
		if value == "*" || value == origin {
			return true
		}
	}
	return false
}

func (s *Server) rejectTraversal(next http.Handler) http.Handler {
	return http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if strings.Contains(request.URL.Path, "..") || strings.Contains(request.URL.Path, "\\") ||
			strings.Contains(request.URL.Path, "\x00") {
			writeError(writer, request, http.StatusBadRequest, "invalid_request", "request path is invalid")
			return
		}
		next.ServeHTTP(writer, request)
	})
}

func (s *Server) recover(next http.Handler) http.Handler {
	return http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		defer func() {
			if value := recover(); value != nil {
				s.logger.Error("panic while serving request",
					"method", request.Method,
					"path", request.URL.Path,
					"request_id", requestID(request),
					"panic", fmt.Sprint(value),
					"stack", string(debug.Stack()))
				writeError(writer, request, http.StatusInternalServerError, "internal", "internal server error")
			}
		}()
		next.ServeHTTP(writer, request)
	})
}

type responseRecorder struct {
	http.ResponseWriter
	status int
	bytes  int64
}

func (r *responseRecorder) WriteHeader(status int) {
	if r.status == 0 {
		r.status = status
		r.ResponseWriter.WriteHeader(status)
	}
}

func (r *responseRecorder) Write(value []byte) (int, error) {
	if r.status == 0 {
		r.status = http.StatusOK
	}
	written, err := r.ResponseWriter.Write(value)
	r.bytes += int64(written)
	return written, err
}

func (s *Server) logging(next http.Handler) http.Handler {
	return http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		start := time.Now()
		recorder := &responseRecorder{ResponseWriter: writer}
		next.ServeHTTP(recorder, request)
		status := recorder.status
		if status == 0 {
			status = http.StatusOK
		}
		pattern := request.Pattern
		if pattern == "" {
			pattern = "unmatched"
		}
		s.metrics.Inc("parcel_http_requests_total", map[string]string{
			"method": request.Method,
			"route":  pattern,
			"status": fmt.Sprintf("%d", status),
		})
		s.logger.Info("http request",
			"method", request.Method,
			"path", request.URL.Path,
			"route", pattern,
			"status", status,
			"bytes", recorder.bytes,
			"duration_ms", time.Since(start).Milliseconds(),
			"request_id", requestID(request))
	})
}

func requestID(request *http.Request) string {
	value, _ := request.Context().Value(contextRequestID).(string)
	return value
}

func (s *Server) clientIP(request *http.Request) string {
	if s.cfg.TrustProxy {
		if forwarded := request.Header.Get("X-Forwarded-For"); forwarded != "" {
			value, _, _ := strings.Cut(forwarded, ",")
			if value = strings.TrimSpace(value); value != "" {
				return value
			}
		}
	}
	host, _, err := net.SplitHostPort(request.RemoteAddr)
	if err != nil {
		return request.RemoteAddr
	}
	return host
}

type rateLimiter struct {
	mu      sync.Mutex
	limit   int
	window  time.Duration
	entries map[string]rateEntry
}

type rateEntry struct {
	count int
	start time.Time
}

func newRateLimiter(limit int, window time.Duration) *rateLimiter {
	if limit <= 0 {
		limit = 1
	}
	if window <= 0 {
		window = time.Minute
	}
	return &rateLimiter{limit: limit, window: window, entries: make(map[string]rateEntry)}
}

func (l *rateLimiter) Allow(key string) bool {
	now := time.Now()
	l.mu.Lock()
	defer l.mu.Unlock()
	entry, ok := l.entries[key]
	if !ok || now.Sub(entry.start) >= l.window {
		l.entries[key] = rateEntry{count: 1, start: now}
		return true
	}
	if entry.count >= l.limit {
		return false
	}
	entry.count++
	l.entries[key] = entry
	if len(l.entries) > 10000 {
		for item, value := range l.entries {
			if now.Sub(value.start) > 2*l.window {
				delete(l.entries, item)
			}
		}
	}
	return true
}

func (s *Server) limit(name string, limit int, next http.HandlerFunc) http.HandlerFunc {
	limiter := newRateLimiter(limit, s.cfg.RateWindow)
	return func(writer http.ResponseWriter, request *http.Request) {
		if !limiter.Allow(name + ":" + s.clientIP(request)) {
			s.metrics.Inc("parcel_rate_limited_total", map[string]string{"route": name})
			writeError(writer, request, http.StatusTooManyRequests, "rate_limited", "rate limit exceeded")
			return
		}
		next(writer, request)
	}
}

func (s *Server) requireAdmin(next func(http.ResponseWriter, *http.Request, *adminIdentity)) http.HandlerFunc {
	return func(writer http.ResponseWriter, request *http.Request) {
		identity, err := s.authenticateAdmin(request)
		if err != nil {
			writeError(writer, request, http.StatusUnauthorized, "unauthorized", "authentication required")
			return
		}
		if !identity.APIKey && request.Method != http.MethodGet && request.Method != http.MethodHead {
			provided := request.Header.Get("X-CSRF-Token")
			if provided == "" || provided != identity.CSRF {
				writeError(writer, request, http.StatusForbidden, "forbidden", "CSRF token is invalid")
				return
			}
		}
		next(writer, request, identity)
	}
}

func (s *Server) authenticateAdmin(request *http.Request) (*adminIdentity, error) {
	if key := strings.TrimSpace(request.Header.Get("X-API-Key")); key != "" {
		user, err := s.store.UserByAPIKeyHash(request.Context(), secure.HashAPIKey(key), time.Now().UTC())
		if err != nil {
			return nil, err
		}
		return &adminIdentity{User: user, APIKey: true}, nil
	}
	cookie, err := request.Cookie(sessionCookie)
	if err != nil || cookie.Value == "" {
		return nil, errors.New("session cookie is missing")
	}
	session, user, err := s.store.SessionByID(request.Context(), cookie.Value, time.Now().UTC())
	if err != nil {
		return nil, err
	}
	return &adminIdentity{User: user, CSRF: session.CSRFToken, SessionID: session.ID}, nil
}

func (s *Server) requireClient(next func(http.ResponseWriter, *http.Request, secure.Claims)) http.HandlerFunc {
	return func(writer http.ResponseWriter, request *http.Request) {
		claims, err := s.authenticateClient(request)
		if err != nil {
			writeError(writer, request, http.StatusUnauthorized, "unauthorized", "client token is invalid or expired")
			return
		}
		next(writer, request, claims)
	}
}

func (s *Server) optionalClient(request *http.Request) (*secure.Claims, error) {
	if secure.BearerToken(request.Header.Get("Authorization")) == "" {
		return nil, nil
	}
	claims, err := s.authenticateClient(request)
	if err != nil {
		return nil, err
	}
	return &claims, nil
}

func (s *Server) authenticateClient(request *http.Request) (secure.Claims, error) {
	token := secure.BearerToken(request.Header.Get("Authorization"))
	if token == "" {
		return secure.Claims{}, errors.New("bearer token is missing")
	}
	return secure.ParseJWT(s.cfg.JWTSecret, token, s.cfg.JWTIssuer, time.Now())
}

func (s *Server) handleHealth(writer http.ResponseWriter, request *http.Request) {
	ctx, cancel := context.WithTimeout(request.Context(), 2*time.Second)
	defer cancel()
	if err := s.store.Ping(ctx); err != nil {
		writeError(writer, request, http.StatusServiceUnavailable, "unhealthy", "database is unavailable")
		return
	}
	writeJSON(writer, http.StatusOK, map[string]any{
		"status":  "ok",
		"version": s.version,
		"time":    time.Now().UTC(),
	})
}

func writeJSON(writer http.ResponseWriter, status int, value any) {
	writer.Header().Set("Content-Type", "application/json; charset=utf-8")
	writer.WriteHeader(status)
	if value == nil {
		return
	}
	_ = json.NewEncoder(writer).Encode(value)
}

func writeError(writer http.ResponseWriter, request *http.Request, status int, code, message string) {
	writeJSON(writer, status, errorBody{Code: code, Message: message, RequestID: requestID(request)})
}

func decodeJSON(writer http.ResponseWriter, request *http.Request, value any, limit int64) bool {
	if limit <= 0 {
		limit = 1 << 20
	}
	request.Body = http.MaxBytesReader(writer, request.Body, limit)
	decoder := json.NewDecoder(request.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(value); err != nil {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid JSON body")
		return false
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "request body must contain one JSON value")
		return false
	}
	return true
}

func parsePagination(request *http.Request) (int, int) {
	page := 1
	size := 50
	if value := request.URL.Query().Get("page"); value != "" {
		if parsed, err := parseInt(value); err == nil && parsed > 0 {
			page = parsed
		}
	}
	if value := request.URL.Query().Get("page_size"); value != "" {
		if parsed, err := parseInt(value); err == nil && parsed > 0 {
			size = parsed
		}
	}
	if size > 200 {
		size = 200
	}
	return page, size
}

func parseInt(value string) (int, error) {
	var parsed int
	_, err := fmt.Sscanf(value, "%d", &parsed)
	return parsed, err
}

func mapStoreError(writer http.ResponseWriter, request *http.Request, err error) {
	switch {
	case errors.Is(err, store.ErrNotFound):
		writeError(writer, request, http.StatusNotFound, "not_found", "resource not found")
	case errors.Is(err, store.ErrConflict):
		writeError(writer, request, http.StatusConflict, "conflict", "resource already exists")
	case errors.Is(err, store.ErrInvalid):
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid request")
	default:
		writeError(writer, request, http.StatusInternalServerError, "internal", "internal server error")
	}
}

func (s *Server) audit(request *http.Request, identity *adminIdentity, action, target, detail string) {
	userID := ""
	if identity != nil {
		userID = identity.User.ID
	}
	if err := s.store.Audit(request.Context(), store.AuditInput{
		UserID:    userID,
		Action:    action,
		Target:    target,
		Detail:    detail,
		RequestID: requestID(request),
	}); err != nil {
		s.logger.Error("audit write failed", "action", action, "target", target, "error", err)
	}
}
