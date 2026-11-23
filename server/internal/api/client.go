package api

import (
	"context"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"net/http"
	"net/url"
	"strconv"
	"strings"

	"parcel/server/internal/secure"
	"parcel/server/internal/store"
)

type clientConfig struct {
	Schema        int            `json:"schema"`
	Package       string         `json:"package"`
	Channel       string         `json:"channel"`
	Version       string         `json:"version"`
	ManifestHash  string         `json:"manifest_hash"`
	CDN           []string       `json:"cdn"`
	SignURL       string         `json:"sign_url"`
	Auth          clientAuth     `json:"auth"`
	Gray          clientGray     `json:"gray"`
	Rollback      clientRollback `json:"rollback"`
	Reason        string         `json:"reason"`
	MinAppVersion string         `json:"min_app_version,omitempty"`
	ForceUpdate   bool           `json:"force_update"`
}

type clientAuth struct {
	Mode     string `json:"mode"`
	Header   string `json:"header"`
	TokenURL string `json:"token_url"`
}

type clientGray struct {
	Salt  string       `json:"salt"`
	Rules []clientRule `json:"rules"`
}

type clientRule struct {
	Version       string   `json:"version"`
	Percentage    int      `json:"percentage"`
	Whitelist     []string `json:"whitelist,omitempty"`
	Platforms     []string `json:"platforms,omitempty"`
	MinAppVersion string   `json:"min_app_version,omitempty"`
	Enabled       bool     `json:"enabled"`
}

type clientRollback struct {
	PreviousVersion string `json:"previous_version,omitempty"`
}

type clientVersionResponse struct {
	Schema        int    `json:"schema"`
	Package       string `json:"package"`
	Channel       string `json:"channel"`
	Version       string `json:"version"`
	ManifestHash  string `json:"manifest_hash"`
	Reason        string `json:"reason"`
	Previous      string `json:"previous_version,omitempty"`
	MinAppVersion string `json:"min_app_version,omitempty"`
	ForceUpdate   bool   `json:"force_update"`
}

type signedURLResponse struct {
	URL       string `json:"url"`
	Expires   int64  `json:"expires"`
	KeyID     string `json:"key_id"`
	Signature string `json:"signature"`
}

func (s *Server) handleClientConfig(writer http.ResponseWriter, request *http.Request) {
	claims, err := s.optionalClient(request)
	if err != nil {
		writeError(writer, request, http.StatusUnauthorized, "unauthorized", "client token is invalid or expired")
		return
	}
	resolution, err := s.resolveClient(request.Context(), request)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	if claims != nil && claims.Package != "" && claims.Package != resolution.Package {
		writeError(writer, request, http.StatusForbidden, "forbidden", "token package does not match")
		return
	}
	base := strings.TrimRight(s.cfg.PublicBaseURL, "/")
	cdn := make([]string, 0, len(s.cfg.CDNURLs)+1)
	paths := url.PathEscape(resolution.Package) + "/" + url.PathEscape(resolution.Version.Version) + "/"
	if len(s.cfg.CDNURLs) == 0 {
		cdn = append(cdn, base+"/files/"+paths)
	} else {
		for _, value := range s.cfg.CDNURLs {
			cdn = append(cdn, strings.TrimRight(value, "/")+"/files/"+paths)
		}
	}
	rules := make([]clientRule, 0, len(resolution.Rules))
	for _, rule := range resolution.Rules {
		if !rule.Enabled {
			continue
		}
		rules = append(rules, clientRule{
			Version:       rule.Version,
			Percentage:    rule.Percentage,
			Whitelist:     rule.Whitelist,
			Platforms:     rule.Platforms,
			MinAppVersion: rule.MinAppVersion,
			Enabled:       rule.Enabled,
		})
	}
	force := false
	if s.cfg.MinAppVersion != "" {
		appVersion := strings.TrimSpace(request.URL.Query().Get("app_version"))
		force = appVersion == "" || CompareVersions(appVersion, s.cfg.MinAppVersion) < 0
	}
	value := clientConfig{
		Schema:       1,
		Package:      resolution.Package,
		Channel:      resolution.Channel,
		Version:      resolution.Version.Version,
		ManifestHash: "sha256:" + resolution.Version.ManifestHash,
		CDN:          cdn,
		SignURL:      base + "/api/v1/client/url",
		Auth: clientAuth{
			Mode:     "token+sign",
			Header:   "Authorization",
			TokenURL: base + "/api/v1/auth/device-token",
		},
		Gray: clientGray{
			Salt:  s.cfg.GraySalt,
			Rules: rules,
		},
		Rollback:      clientRollback{PreviousVersion: resolution.Previous},
		Reason:        resolution.Decision,
		MinAppVersion: s.cfg.MinAppVersion,
		ForceUpdate:   force,
	}
	s.metrics.Inc("parcel_gray_decisions_total", map[string]string{
		"package": resolution.Package,
		"channel": resolution.Channel,
		"reason":  resolution.Decision,
	})
	writeJSON(writer, http.StatusOK, value)
}

func (s *Server) handleClientVersion(writer http.ResponseWriter, request *http.Request) {
	claims, err := s.optionalClient(request)
	if err != nil {
		writeError(writer, request, http.StatusUnauthorized, "unauthorized", "client token is invalid or expired")
		return
	}
	resolution, err := s.resolveClient(request.Context(), request)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	if claims != nil && claims.Package != "" && claims.Package != resolution.Package {
		writeError(writer, request, http.StatusForbidden, "forbidden", "token package does not match")
		return
	}
	force := false
	if s.cfg.MinAppVersion != "" {
		appVersion := strings.TrimSpace(request.URL.Query().Get("app_version"))
		force = appVersion == "" || CompareVersions(appVersion, s.cfg.MinAppVersion) < 0
	}
	writeJSON(writer, http.StatusOK, clientVersionResponse{
		Schema:        1,
		Package:       resolution.Package,
		Channel:       resolution.Channel,
		Version:       resolution.Version.Version,
		ManifestHash:  "sha256:" + resolution.Version.ManifestHash,
		Reason:        resolution.Decision,
		Previous:      resolution.Previous,
		MinAppVersion: s.cfg.MinAppVersion,
		ForceUpdate:   force,
	})
}

func (s *Server) handleClientURL(writer http.ResponseWriter, request *http.Request, claims secure.Claims) {
	value := strings.TrimSpace(request.URL.Query().Get("path"))
	signedPath, err := secure.NormalizeSignedPath(value)
	if err != nil {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "path is invalid")
		return
	}
	parts := strings.Split(strings.TrimPrefix(signedPath, "/files/"), "/")
	if len(parts) < 3 {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "path is invalid")
		return
	}
	if claims.Package != "" && parts[0] != claims.Package {
		writeError(writer, request, http.StatusForbidden, "forbidden", "token package does not match")
		return
	}
	if _, err := s.resolveFilePath(request.Context(), signedPath); err != nil {
		if errors.Is(err, store.ErrInvalid) {
			writeError(writer, request, http.StatusBadRequest, "invalid_request", "path is invalid")
			return
		}
		writeError(writer, request, http.StatusNotFound, "not_found", "file not found")
		return
	}
	keyID, expires, signature := s.signer.Sign(signedPath)
	query := url.Values{}
	query.Set("expires", strconv.FormatInt(expires, 10))
	query.Set("key_id", keyID)
	query.Set("signature", signature)
	writeJSON(writer, http.StatusOK, signedURLResponse{
		URL:       strings.TrimRight(s.cfg.PublicBaseURL, "/") + signedPath + "?" + query.Encode(),
		Expires:   expires,
		KeyID:     keyID,
		Signature: signature,
	})
}

func (s *Server) resolveClient(ctx context.Context, request *http.Request) (store.Resolution, error) {
	query := request.URL.Query()
	packageName := strings.TrimSpace(query.Get("package"))
	channel := strings.TrimSpace(query.Get("channel"))
	playerID := strings.TrimSpace(query.Get("player_id"))
	platform := strings.TrimSpace(query.Get("platform"))
	appVersion := strings.TrimSpace(query.Get("app_version"))
	if channel == "" {
		channel = "prod"
	}
	if !safePackage.MatchString(packageName) || !safeComponent(channel, 64) {
		return store.Resolution{}, store.ErrInvalid
	}
	if len(playerID) > 128 || len(platform) > 64 || len(appVersion) > 64 {
		return store.Resolution{}, store.ErrInvalid
	}
	ch, err := s.store.ChannelByName(ctx, packageName, channel)
	if err != nil {
		return store.Resolution{}, err
	}
	if ch.Version == "" {
		return store.Resolution{}, store.ErrNotFound
	}
	version, err := s.store.GetVersion(ctx, packageName, ch.Version)
	if err != nil {
		return store.Resolution{}, err
	}
	if version.ManifestHash == "" {
		return store.Resolution{}, store.ErrNotFound
	}
	rules, err := s.store.ListGrayRules(ctx, packageName)
	if err != nil {
		return store.Resolution{}, err
	}
	previous := ""
	if ch.VersionID != nil {
		previous, err = s.store.PreviousVersion(ctx, packageName, *ch.VersionID)
		if err != nil {
			return store.Resolution{}, err
		}
	}
	selected := version
	reason := "channel"
	for _, rule := range rules {
		if !rule.Enabled {
			continue
		}
		if len(rule.Platforms) > 0 && !contains(rule.Platforms, platform) {
			continue
		}
		if rule.MinAppVersion != "" && (appVersion == "" || CompareVersions(appVersion, rule.MinAppVersion) < 0) {
			continue
		}
		matched := contains(rule.Whitelist, playerID)
		if !matched && rule.Percentage > 0 {
			matched = int(PercentageBucket(playerID, packageName, s.cfg.GraySalt)) < rule.Percentage*100
		}
		if !matched {
			continue
		}
		candidate, err := s.store.GetVersion(ctx, packageName, rule.Version)
		if err != nil || candidate.ManifestHash == "" {
			continue
		}
		selected = candidate
		reason = "gray"
		break
	}
	s.metrics.Inc("parcel_gray_hits_total", map[string]string{
		"package": packageName,
		"channel": channel,
		"version": selected.Version,
	})
	return store.Resolution{
		Package:  packageName,
		Channel:  channel,
		Version:  selected,
		Previous: previous,
		Rules:    rules,
		Decision: reason,
	}, nil
}

func PercentageBucket(playerID, packageName, salt string) uint32 {
	sum := sha256.Sum256([]byte(playerID + packageName + salt))
	value := binary.BigEndian.Uint32(sum[:4])
	return value % 10000
}

func contains(values []string, target string) bool {
	for _, value := range values {
		if value == target {
			return true
		}
	}
	return false
}

func CompareVersions(left, right string) int {
	parse := func(value string) []int {
		value = strings.TrimPrefix(strings.TrimSpace(value), "v")
		parts := strings.Split(value, ".")
		result := make([]int, 0, len(parts))
		for _, part := range parts {
			digits := part
			for index, char := range part {
				if char < '0' || char > '9' {
					digits = part[:index]
					break
				}
			}
			if digits == "" {
				result = append(result, 0)
				continue
			}
			number, _ := strconv.Atoi(digits)
			result = append(result, number)
		}
		return result
	}
	leftParts := parse(left)
	rightParts := parse(right)
	count := len(leftParts)
	if len(rightParts) > count {
		count = len(rightParts)
	}
	for index := 0; index < count; index++ {
		leftValue, rightValue := 0, 0
		if index < len(leftParts) {
			leftValue = leftParts[index]
		}
		if index < len(rightParts) {
			rightValue = rightParts[index]
		}
		if leftValue < rightValue {
			return -1
		}
		if leftValue > rightValue {
			return 1
		}
	}
	return 0
}
