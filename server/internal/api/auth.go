package api

import (
	"errors"
	"net/http"
	"strings"
	"time"

	"parcel/server/internal/secure"
	"parcel/server/internal/store"
)

const sessionCookie = "parcel_session"

type loginRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

type deviceTokenRequest struct {
	PlayerID string `json:"player_id"`
	Package  string `json:"package"`
	Platform string `json:"platform"`
}

func (s *Server) handleLogin(writer http.ResponseWriter, request *http.Request) {
	var body loginRequest
	if !decodeJSON(writer, request, &body, 64<<10) {
		return
	}
	body.Username = strings.TrimSpace(body.Username)
	if body.Username == "" || body.Password == "" || len(body.Password) > 1024 {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "username and password are required")
		return
	}
	user, err := s.store.UserByUsername(request.Context(), body.Username)
	if err != nil {
		secure.BurnPassword(body.Password)
		s.metrics.Inc("parcel_auth_failures_total", map[string]string{"kind": "login"})
		writeError(writer, request, http.StatusUnauthorized, "unauthorized", "invalid credentials")
		return
	}
	if !secure.CheckPassword(user.PasswordHash, body.Password) {
		s.metrics.Inc("parcel_auth_failures_total", map[string]string{"kind": "login"})
		writeError(writer, request, http.StatusUnauthorized, "unauthorized", "invalid credentials")
		return
	}
	sessionID, err := secure.RandomToken(32)
	if err != nil {
		writeError(writer, request, http.StatusInternalServerError, "internal", "could not create session")
		return
	}
	csrf, err := secure.RandomToken(24)
	if err != nil {
		writeError(writer, request, http.StatusInternalServerError, "internal", "could not create session")
		return
	}
	expires := time.Now().UTC().Add(s.cfg.SessionTTL)
	if err := s.store.CreateSession(request.Context(), sessionID, user.ID, csrf, expires); err != nil {
		writeError(writer, request, http.StatusInternalServerError, "internal", "could not create session")
		return
	}
	http.SetCookie(writer, &http.Cookie{
		Name:     sessionCookie,
		Value:    sessionID,
		Path:     "/",
		HttpOnly: true,
		Secure:   s.cfg.SecureCookies,
		SameSite: http.SameSiteLaxMode,
		Expires:  expires,
		MaxAge:   int(s.cfg.SessionTTL.Seconds()),
	})
	s.audit(request, &adminIdentity{User: user}, "auth.login", user.Username, "")
	writeJSON(writer, http.StatusOK, map[string]any{
		"user":       publicUser(user),
		"csrf_token": csrf,
	})
}

func (s *Server) handleLogout(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	if identity.APIKey {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "API key sessions cannot be logged out")
		return
	}
	_ = s.store.DeleteSession(request.Context(), identity.SessionID)
	http.SetCookie(writer, &http.Cookie{
		Name:     sessionCookie,
		Value:    "",
		Path:     "/",
		HttpOnly: true,
		Secure:   s.cfg.SecureCookies,
		SameSite: http.SameSiteLaxMode,
		MaxAge:   -1,
	})
	s.audit(request, identity, "auth.logout", identity.User.Username, "")
	writer.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleMe(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	writeJSON(writer, http.StatusOK, map[string]any{
		"user":       publicUser(identity.User),
		"csrf_token": identity.CSRF,
		"api_key":    identity.APIKey,
	})
}

func publicUser(user store.User) map[string]any {
	return map[string]any{
		"id":         user.ID,
		"username":   user.Username,
		"role":       user.Role,
		"created_at": user.CreatedAt,
	}
}

type createAPIKeyRequest struct {
	Name     string `json:"name"`
	ExpiresH int    `json:"expires_in_hours"`
}

func (s *Server) handleCreateAPIKey(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	if identity.User.Role != "admin" {
		writeError(writer, request, http.StatusForbidden, "forbidden", "admin role is required")
		return
	}
	var body createAPIKeyRequest
	if !decodeJSON(writer, request, &body, 64<<10) {
		return
	}
	body.Name = strings.TrimSpace(body.Name)
	if body.Name == "" || len(body.Name) > 100 {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "API key name is required")
		return
	}
	if body.ExpiresH < 0 || body.ExpiresH > 24*365 {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "expiry is out of range")
		return
	}
	token, err := secure.RandomToken(32)
	if err != nil {
		writeError(writer, request, http.StatusInternalServerError, "internal", "could not create API key")
		return
	}
	token = "parcel_" + token
	var expires *time.Time
	if body.ExpiresH > 0 {
		value := time.Now().UTC().Add(time.Duration(body.ExpiresH) * time.Hour)
		expires = &value
	}
	id, err := s.store.CreateAPIKey(request.Context(), identity.User.ID, body.Name, secure.HashAPIKey(token), expires)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	s.audit(request, identity, "apikey.create", id, body.Name)
	writeJSON(writer, http.StatusCreated, map[string]any{
		"id":      id,
		"name":    body.Name,
		"api_key": token,
		"expires": expires,
	})
}

func (s *Server) handleDeviceToken(writer http.ResponseWriter, request *http.Request) {
	var body deviceTokenRequest
	if !decodeJSON(writer, request, &body, 64<<10) {
		return
	}
	body.PlayerID = strings.TrimSpace(body.PlayerID)
	body.Package = strings.TrimSpace(body.Package)
	body.Platform = strings.TrimSpace(body.Platform)
	if body.PlayerID == "" || len(body.PlayerID) > 128 || len(body.Package) > 64 || len(body.Platform) > 64 {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "player_id is required")
		return
	}
	if body.Package != "" {
		if _, err := s.store.PackageByName(request.Context(), body.Package); err != nil {
			if errors.Is(err, store.ErrNotFound) {
				writeError(writer, request, http.StatusNotFound, "not_found", "package not found")
				return
			}
			mapStoreError(writer, request, err)
			return
		}
	}
	now := time.Now().UTC()
	claims, err := secure.NewClaims(body.PlayerID, body.PlayerID, body.Package, body.Platform,
		s.cfg.JWTIssuer, now, s.cfg.TokenTTL)
	if err != nil {
		writeError(writer, request, http.StatusInternalServerError, "internal", "could not issue token")
		return
	}
	token, err := secure.SignJWT(s.cfg.JWTSecret, claims)
	if err != nil {
		writeError(writer, request, http.StatusInternalServerError, "internal", "could not issue token")
		return
	}
	if err := s.store.SaveDeviceToken(request.Context(), body.PlayerID, secure.HashAPIKey(token),
		time.Unix(claims.Expires, 0).UTC()); err != nil {
		writeError(writer, request, http.StatusInternalServerError, "internal", "could not issue token")
		return
	}
	s.metrics.Inc("parcel_device_tokens_total", map[string]string{"package": body.Package})
	writeJSON(writer, http.StatusOK, map[string]any{
		"token":      token,
		"token_type": "Bearer",
		"expires_in": claims.ExpiresIn(now),
		"expires_at": time.Unix(claims.Expires, 0).UTC(),
	})
}

func (s *Server) handleOverview(writer http.ResponseWriter, request *http.Request, _ *adminIdentity) {
	counts, err := s.store.Counts(request.Context())
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	writeJSON(writer, http.StatusOK, map[string]any{
		"counts":  counts,
		"metrics": s.metrics.Snapshot(),
	})
}

func (s *Server) handleMetrics(writer http.ResponseWriter, request *http.Request, _ *adminIdentity) {
	writeJSON(writer, http.StatusOK, s.metrics.Snapshot())
}

func (s *Server) handleAudit(writer http.ResponseWriter, request *http.Request, _ *adminIdentity) {
	page, size := parsePagination(request)
	items, total, err := s.store.ListAudit(request.Context(), size, (page-1)*size)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	writeJSON(writer, http.StatusOK, pagination{Items: items, Total: total, Page: page, PageSize: size})
}
