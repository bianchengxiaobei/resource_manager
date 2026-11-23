package secure

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
)

var (
	ErrInvalidToken = errors.New("invalid token")
	ErrExpiredToken = errors.New("token expired")
)

type Claims struct {
	Subject  string `json:"sub"`
	PlayerID string `json:"player_id,omitempty"`
	Package  string `json:"package,omitempty"`
	Platform string `json:"platform,omitempty"`
	Issuer   string `json:"iss"`
	IssuedAt int64  `json:"iat"`
	Expires  int64  `json:"exp"`
	ID       string `json:"jti"`
}

type jwtHeader struct {
	Alg string `json:"alg"`
	Typ string `json:"typ"`
}

func SignJWT(secret []byte, claims Claims) (string, error) {
	if len(secret) == 0 {
		return "", errors.New("JWT secret is empty")
	}
	header, err := json.Marshal(jwtHeader{Alg: "HS256", Typ: "JWT"})
	if err != nil {
		return "", err
	}
	payload, err := json.Marshal(claims)
	if err != nil {
		return "", err
	}
	encodedHeader := base64.RawURLEncoding.EncodeToString(header)
	encodedPayload := base64.RawURLEncoding.EncodeToString(payload)
	signingInput := encodedHeader + "." + encodedPayload
	mac := hmac.New(sha256.New, secret)
	_, _ = mac.Write([]byte(signingInput))
	return signingInput + "." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil)), nil
}

func ParseJWT(secret []byte, value string, issuer string, now time.Time) (Claims, error) {
	var claims Claims
	parts := strings.Split(value, ".")
	if len(parts) != 3 {
		return claims, ErrInvalidToken
	}
	headerBytes, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return claims, ErrInvalidToken
	}
	var header jwtHeader
	if err := json.Unmarshal(headerBytes, &header); err != nil || header.Alg != "HS256" || header.Typ != "JWT" {
		return claims, ErrInvalidToken
	}
	mac := hmac.New(sha256.New, secret)
	_, _ = mac.Write([]byte(parts[0] + "." + parts[1]))
	signature, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil || !hmac.Equal(signature, mac.Sum(nil)) {
		return claims, ErrInvalidToken
	}
	payload, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return claims, ErrInvalidToken
	}
	if err := json.Unmarshal(payload, &claims); err != nil {
		return claims, ErrInvalidToken
	}
	if claims.Subject == "" || claims.ID == "" || claims.Issuer == "" {
		return claims, ErrInvalidToken
	}
	if issuer != "" && claims.Issuer != issuer {
		return claims, ErrInvalidToken
	}
	if claims.Expires <= now.Unix() {
		return claims, ErrExpiredToken
	}
	if claims.IssuedAt > now.Add(5*time.Minute).Unix() {
		return claims, ErrInvalidToken
	}
	return claims, nil
}

func NewClaims(subject, playerID, packageName, platform, issuer string, now time.Time, ttl time.Duration) (Claims, error) {
	id, err := RandomToken(16)
	if err != nil {
		return Claims{}, err
	}
	return Claims{
		Subject:  subject,
		PlayerID: playerID,
		Package:  packageName,
		Platform: platform,
		Issuer:   issuer,
		IssuedAt: now.Unix(),
		Expires:  now.Add(ttl).Unix(),
		ID:       id,
	}, nil
}

func BearerToken(value string) string {
	const prefix = "Bearer "
	if len(value) > len(prefix) && strings.EqualFold(value[:len(prefix)], prefix) {
		return strings.TrimSpace(value[len(prefix):])
	}
	return ""
}

func (c Claims) ExpiresIn(now time.Time) int64 {
	value := c.Expires - now.Unix()
	if value < 0 {
		return 0
	}
	return value
}

func (c Claims) String() string {
	return fmt.Sprintf("sub=%s exp=%s", c.Subject, time.Unix(c.Expires, 0).UTC().Format(time.RFC3339))
}
