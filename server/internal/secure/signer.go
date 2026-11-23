package secure

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"errors"
	"strconv"
	"strings"
	"time"
)

type Signer struct {
	keys      map[string][]byte
	defaultID string
	ttl       time.Duration
	now       func() time.Time
}

func NewSigner(keys map[string][]byte, defaultID string, ttl time.Duration) (*Signer, error) {
	if len(keys) == 0 {
		return nil, errors.New("no HMAC keys configured")
	}
	if len(keys[defaultID]) == 0 {
		return nil, errors.New("default HMAC key is not configured")
	}
	return &Signer{keys: keys, defaultID: defaultID, ttl: ttl, now: time.Now}, nil
}

func (s *Signer) TTL() time.Duration { return s.ttl }

func (s *Signer) Sign(path string) (keyID string, expires int64, signature string) {
	keyID = s.defaultID
	expires = s.now().Add(s.ttl).Unix()
	signature = s.sign(keyID, path, expires)
	return keyID, expires, signature
}

func (s *Signer) Verify(path string, expires int64, keyID, signature string) error {
	if keyID == "" || signature == "" {
		return errors.New("signature is incomplete")
	}
	key, ok := s.keys[keyID]
	if !ok {
		return errors.New("signature key is unknown")
	}
	now := s.now().Unix()
	if expires <= now {
		return errors.New("signature expired")
	}
	if expires > now+int64((7*24*time.Hour).Seconds()) {
		return errors.New("signature expiry is too far in the future")
	}
	expected := s.signWithKey(key, keyID, path, expires)
	actual, err := base64.RawURLEncoding.DecodeString(signature)
	if err != nil {
		return errors.New("signature encoding is invalid")
	}
	expectedBytes, err := base64.RawURLEncoding.DecodeString(expected)
	if err != nil {
		return errors.New("signature encoding is invalid")
	}
	if !hmac.Equal(actual, expectedBytes) {
		return errors.New("signature mismatch")
	}
	return nil
}

func (s *Signer) sign(keyID, path string, expires int64) string {
	return s.signWithKey(s.keys[keyID], keyID, path, expires)
}

func (s *Signer) signWithKey(key []byte, keyID, path string, expires int64) string {
	mac := hmac.New(sha256.New, key)
	_, _ = mac.Write([]byte(canonical(keyID, path, expires)))
	return base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
}

func canonical(keyID, path string, expires int64) string {
	return keyID + "\n" + path + "\n" + strconv.FormatInt(expires, 10)
}

func NormalizeSignedPath(value string) (string, error) {
	if value == "" || !strings.HasPrefix(value, "/files/") {
		return "", errors.New("signed path must start with /files/")
	}
	if strings.Contains(value, "..") || strings.Contains(value, "\\") || strings.Contains(value, "\x00") {
		return "", errors.New("signed path is unsafe")
	}
	return value, nil
}
