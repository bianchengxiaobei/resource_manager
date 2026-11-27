package api

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"net/http/cookiejar"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"parcel/server/internal/blob"
	"parcel/server/internal/config"
	"parcel/server/internal/secure"
	"parcel/server/internal/store"
)

type testEnvironment struct {
	t       *testing.T
	server  *Server
	http    *httptest.Server
	client  *http.Client
	store   *store.Store
	cfg     config.Config
	baseDir string
}

func newTestEnvironment(t *testing.T) *testEnvironment {
	return newTestEnvironmentWith(t, nil)
}

func newTestEnvironmentWith(t *testing.T, mutate func(*config.Config)) *testEnvironment {
	t.Helper()
	base := t.TempDir()
	cfg := config.Config{
		Listen:             ":0",
		DBPath:             filepath.Join(base, "parcel.db"),
		BlobRoot:           filepath.Join(base, "blobs"),
		PublicBaseURL:      "http://example.test",
		JWTSecret:          []byte("test-jwt-secret"),
		JWTIssuer:          "parcel-test",
		HMACKeys:           map[string][]byte{"test": []byte("test-hmac-secret")},
		HMACKeyID:          "test",
		GraySalt:           "test-salt",
		SessionTTL:         time.Hour,
		TokenTTL:           time.Hour,
		SignedURLTTL:       time.Hour,
		MaxUploadBytes:     1 << 20,
		MaxManifestBytes:   1 << 20,
		MaxBundleBytes:     1 << 20,
		RequireSignedFiles: true,
		AdminUsername:      "admin",
		RateLimitLogin:     100,
		RateLimitToken:     100,
		RateLimitUpload:    100,
		RateLimitDownload:  1000,
		RateLimitDefault:   1000,
		RateWindow:         time.Minute,
		ReadHeaderTimeout:  time.Second,
		ReadTimeout:        time.Second,
		WriteTimeout:       time.Second,
		IdleTimeout:        time.Second,
	}
	if mutate != nil {
		mutate(&cfg)
	}
	if err := os.MkdirAll(cfg.BlobRoot, 0o755); err != nil {
		t.Fatal(err)
	}
	st, err := store.Open(cfg.DBPath)
	if err != nil {
		t.Fatal(err)
	}
	hash, err := secure.HashPassword("password")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := st.BootstrapAdmin(t.Context(), "admin", hash); err != nil {
		t.Fatal(err)
	}
	blobs, err := blob.New(cfg.BlobRoot)
	if err != nil {
		t.Fatal(err)
	}
	server, err := New(cfg, st, blobs, nil, nil, "test")
	if err != nil {
		t.Fatal(err)
	}
	httpServer := httptest.NewServer(server.Handler())
	jar, _ := cookiejar.New(nil)
	env := &testEnvironment{
		t:       t,
		server:  server,
		http:    httpServer,
		client:  &http.Client{Jar: jar, Timeout: 10 * time.Second},
		store:   st,
		cfg:     cfg,
		baseDir: base,
	}
	t.Cleanup(func() {
		httpServer.Close()
		_ = st.Close()
	})
	return env
}

func (e *testEnvironment) request(method, path string, body io.Reader, headers map[string]string) *http.Response {
	e.t.Helper()
	request, err := http.NewRequestWithContext(e.t.Context(), method, e.http.URL+path, body)
	if err != nil {
		e.t.Fatal(err)
	}
	for key, value := range headers {
		request.Header.Set(key, value)
	}
	response, err := e.client.Do(request)
	if err != nil {
		e.t.Fatal(err)
	}
	return response
}

func decode[T any](t *testing.T, response *http.Response) T {
	t.Helper()
	defer response.Body.Close()
	var value T
	decoder := json.NewDecoder(response.Body)
	if err := decoder.Decode(&value); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	return value
}

type loginResponse struct {
	User struct {
		ID       string `json:"id"`
		Username string `json:"username"`
		Role     string `json:"role"`
	} `json:"user"`
	CSRF string `json:"csrf_token"`
}

func (e *testEnvironment) login() loginResponse {
	e.t.Helper()
	payload, _ := json.Marshal(map[string]string{"username": "admin", "password": "password"})
	response := e.request(http.MethodPost, "/api/v1/admin/login", bytes.NewReader(payload),
		map[string]string{"Content-Type": "application/json"})
	if response.StatusCode != http.StatusOK {
		e.t.Fatalf("login status %d", response.StatusCode)
	}
	return decode[loginResponse](e.t, response)
}

func (e *testEnvironment) createPackage(csrf, name string) store.Package {
	e.t.Helper()
	payload, _ := json.Marshal(map[string]string{"name": name})
	response := e.request(http.MethodPost, "/api/v1/admin/packages", bytes.NewReader(payload),
		map[string]string{"Content-Type": "application/json", "X-CSRF-Token": csrf})
	if response.StatusCode != http.StatusCreated {
		body, _ := io.ReadAll(response.Body)
		response.Body.Close()
		e.t.Fatalf("create package status %d: %s", response.StatusCode, body)
	}
	return decode[store.Package](e.t, response)
}

func makeManifest(packageName, version string) []byte {
	payload := []byte("payload-" + packageName + "-" + version)
	value := make([]byte, manifestHeaderSize+len(payload))
	copy(value[:4], "PRCM")
	binary.LittleEndian.PutUint16(value[4:6], 1)
	binary.LittleEndian.PutUint16(value[6:8], manifestHeaderSize)
	sum := sha256.Sum256(payload)
	copy(value[96:128], sum[:])
	copy(value[manifestHeaderSize:], payload)
	return value
}

func makeBundle(seed string) []byte {
	value := make([]byte, 128+len(seed))
	copy(value[:4], "PRCL")
	binary.LittleEndian.PutUint16(value[4:6], 1)
	binary.LittleEndian.PutUint16(value[6:8], 128)
	copy(value[128:], seed)
	return value
}

func (e *testEnvironment) upload(csrf, packageName, version string, manifest []byte, bundles map[string][]byte) store.Version {
	e.t.Helper()
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	_ = writer.WriteField("version", version)
	_ = writer.WriteField("note", "test")
	part, err := writer.CreateFormFile("manifest", "manifest.bytes")
	if err != nil {
		e.t.Fatal(err)
	}
	_, _ = part.Write(manifest)
	for name, data := range bundles {
		part, err := writer.CreateFormFile("bundles", name)
		if err != nil {
			e.t.Fatal(err)
		}
		_, _ = part.Write(data)
	}
	_ = writer.Close()
	response := e.request(http.MethodPost, "/api/v1/admin/packages/"+packageName+"/versions",
		&body, map[string]string{"Content-Type": writer.FormDataContentType(), "X-CSRF-Token": csrf})
	if response.StatusCode != http.StatusCreated {
		payload, _ := io.ReadAll(response.Body)
		response.Body.Close()
		e.t.Fatalf("upload status %d: %s", response.StatusCode, payload)
	}
	return decode[store.Version](e.t, response)
}

func (e *testEnvironment) publish(csrf, packageName, version string) store.Channel {
	e.t.Helper()
	payload, _ := json.Marshal(map[string]string{"channel": "prod"})
	response := e.request(http.MethodPost,
		"/api/v1/admin/packages/"+packageName+"/versions/"+version+"/publish",
		bytes.NewReader(payload), map[string]string{"Content-Type": "application/json", "X-CSRF-Token": csrf})
	if response.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(response.Body)
		response.Body.Close()
		e.t.Fatalf("publish status %d: %s", response.StatusCode, body)
	}
	return decode[store.Channel](e.t, response)
}

func TestAdminUploadPublishAndSignedDownload(t *testing.T) {
	env := newTestEnvironment(t)
	login := env.login()
	pkg := env.createPackage(login.CSRF, "game")
	if pkg.Name != "game" {
		t.Fatalf("package name = %q", pkg.Name)
	}
	bundle := makeBundle("bundle-one")
	manifest := makeManifest("game", "1.0.0")
	version := env.upload(login.CSRF, "game", "1.0.0", manifest, map[string][]byte{"one.bundle": bundle})
	if version.ManifestHash == "" {
		t.Fatal("manifest hash is empty")
	}
	wantHash := sha256.Sum256(manifest[manifestHeaderSize:])
	if version.ManifestHash != hex.EncodeToString(wantHash[:]) {
		t.Fatalf("manifest hash = %s", version.ManifestHash)
	}
	channel := env.publish(login.CSRF, "game", "1.0.0")
	if channel.Version != "1.0.0" {
		t.Fatalf("channel version = %q", channel.Version)
	}
	configResponse := env.request(http.MethodGet,
		"/api/v1/client/config?package=game&channel=prod&platform=windows&player_id=p1&app_version=1.0.0",
		nil, nil)
	if configResponse.StatusCode != http.StatusOK {
		t.Fatalf("config status %d", configResponse.StatusCode)
	}
	config := decode[clientConfig](t, configResponse)
	if config.Version != "1.0.0" || config.ManifestHash != "sha256:"+version.ManifestHash {
		t.Fatalf("client config = %+v", config)
	}
	if config.SignURL != "http://example.test/api/v1/client/url" {
		t.Fatalf("client sign URL = %q", config.SignURL)
	}

	tokenResponse := env.request(http.MethodPost, "/api/v1/auth/device-token",
		strings.NewReader(`{"player_id":"p1","package":"game","platform":"windows"}`),
		map[string]string{"Content-Type": "application/json"})
	if tokenResponse.StatusCode != http.StatusOK {
		t.Fatalf("token status %d", tokenResponse.StatusCode)
	}
	token := decode[struct {
		Token string `json:"token"`
	}](t, tokenResponse)
	signedResponse := env.request(http.MethodGet,
		"/api/v1/client/url?path="+url.QueryEscape("/files/game/1.0.0/game_1.0.0.bytes"), nil,
		map[string]string{"Authorization": "Bearer " + token.Token})
	if signedResponse.StatusCode != http.StatusOK {
		t.Fatalf("signed URL status %d", signedResponse.StatusCode)
	}
	var signed signedURLResponse
	signed = decode[signedURLResponse](t, signedResponse)
	signed.URL = strings.Replace(signed.URL, "http://example.test", env.http.URL, 1)
	download, err := http.Get(signed.URL)
	if err != nil {
		t.Fatal(err)
	}
	data, _ := io.ReadAll(download.Body)
	download.Body.Close()
	if download.StatusCode != http.StatusOK || !bytes.Equal(data, manifest) {
		t.Fatalf("download status %d len %d", download.StatusCode, len(data))
	}
	rangeRequest, _ := http.NewRequest(http.MethodGet, signed.URL, nil)
	rangeRequest.Header.Set("Range", "bytes=0-9")
	rangeResponse, err := http.DefaultClient.Do(rangeRequest)
	if err != nil {
		t.Fatal(err)
	}
	if rangeResponse.StatusCode != http.StatusPartialContent {
		t.Fatalf("range status %d", rangeResponse.StatusCode)
	}
	rangeResponse.Body.Close()

	unsigned, err := http.Get(env.http.URL + "/files/game/1.0.0/game_1.0.0.bytes")
	if err != nil {
		t.Fatal(err)
	}
	unsigned.Body.Close()
	if unsigned.StatusCode != http.StatusForbidden {
		t.Fatalf("unsigned status %d", unsigned.StatusCode)
	}
}

func TestGrayRuleAndRollback(t *testing.T) {
	env := newTestEnvironment(t)
	login := env.login()
	env.createPackage(login.CSRF, "game")
	env.upload(login.CSRF, "game", "1.0.0", makeManifest("game", "1.0.0"), nil)
	env.publish(login.CSRF, "game", "1.0.0")
	env.upload(login.CSRF, "game", "2.0.0", makeManifest("game", "2.0.0"), nil)
	env.publish(login.CSRF, "game", "2.0.0")

	grayBody := map[string]any{
		"version": "2.0.0", "percentage": 100, "priority": 10, "enabled": true,
		"platforms": []string{"windows"},
	}
	payload, _ := json.Marshal(grayBody)
	response := env.request(http.MethodPost, "/api/v1/admin/packages/game/gray-rules",
		bytes.NewReader(payload), map[string]string{"Content-Type": "application/json", "X-CSRF-Token": login.CSRF})
	if response.StatusCode != http.StatusCreated {
		body, _ := io.ReadAll(response.Body)
		response.Body.Close()
		t.Fatalf("gray status %d: %s", response.StatusCode, body)
	}
	response.Body.Close()
	configResponse := env.request(http.MethodGet,
		"/api/v1/client/config?package=game&channel=prod&platform=windows&player_id=p1&app_version=2.0.0",
		nil, nil)
	client := decode[clientConfig](t, configResponse)
	if client.Version != "2.0.0" || client.Reason != "gray" {
		t.Fatalf("gray config = %+v", client)
	}
	rollbackPayload := strings.NewReader(`{"channel":"prod","version":"1.0.0"}`)
	rollback := env.request(http.MethodPost, "/api/v1/admin/packages/game/rollback", rollbackPayload,
		map[string]string{"Content-Type": "application/json", "X-CSRF-Token": login.CSRF})
	if rollback.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(rollback.Body)
		rollback.Body.Close()
		t.Fatalf("rollback status %d: %s", rollback.StatusCode, body)
	}
	var channel store.Channel
	channel = decode[store.Channel](t, rollback)
	if channel.Version != "1.0.0" {
		t.Fatalf("rollback version = %q", channel.Version)
	}
}

func TestAdminAuthCSRFAndTraversal(t *testing.T) {
	env := newTestEnvironment(t)
	login := env.login()
	noCSRF := env.request(http.MethodPost, "/api/v1/admin/packages",
		strings.NewReader(`{"name":"bad"}`), map[string]string{"Content-Type": "application/json"})
	if noCSRF.StatusCode != http.StatusForbidden {
		t.Fatalf("missing CSRF status %d", noCSRF.StatusCode)
	}
	noCSRF.Body.Close()
	unauthorized, err := http.Get(env.http.URL + "/api/v1/admin/packages")
	if err != nil {
		t.Fatal(err)
	}
	if unauthorized.StatusCode != http.StatusUnauthorized {
		t.Fatalf("unauthorized status %d", unauthorized.StatusCode)
	}
	unauthorized.Body.Close()
	env.createPackage(login.CSRF, "game")
	env.upload(login.CSRF, "game", "1.0.0", makeManifest("game", "1.0.0"), nil)
	env.publish(login.CSRF, "game", "1.0.0")
	for _, target := range []string{
		"/files/game/1.0.0/../../etc/passwd",
		"/files/game/1.0.0/%2e%2e%2fetc%2fpasswd",
	} {
		response, err := env.client.Get(env.http.URL + target)
		if err != nil {
			t.Fatal(err)
		}
		body, _ := io.ReadAll(response.Body)
		response.Body.Close()
		if response.StatusCode == http.StatusOK || bytes.Contains(body, []byte("passwd")) {
			t.Fatalf("traversal %s returned %d", target, response.StatusCode)
		}
	}
}

func (e *testEnvironment) uploadNoFail(csrf, packageName, version string, manifest []byte) error {
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	_ = writer.WriteField("version", version)
	part, err := writer.CreateFormFile("manifest", "manifest.bytes")
	if err != nil {
		return err
	}
	if _, err := part.Write(manifest); err != nil {
		return err
	}
	if err := writer.Close(); err != nil {
		return err
	}
	request, err := http.NewRequestWithContext(e.t.Context(), http.MethodPost,
		e.http.URL+"/api/v1/admin/packages/"+packageName+"/versions", &body)
	if err != nil {
		return err
	}
	request.Header.Set("Content-Type", writer.FormDataContentType())
	request.Header.Set("X-CSRF-Token", csrf)
	response, err := e.client.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusCreated {
		payload, _ := io.ReadAll(response.Body)
		return fmt.Errorf("upload status %d: %s", response.StatusCode, payload)
	}
	_, _ = io.Copy(io.Discard, response.Body)
	return nil
}

func TestConcurrentUploadsAndDownloads(t *testing.T) {
	env := newTestEnvironment(t)
	login := env.login()
	env.createPackage(login.CSRF, "game")

	const uploaders = 4
	var uploadWG sync.WaitGroup
	uploadErrors := make(chan error, uploaders)
	for index := 0; index < uploaders; index++ {
		uploadWG.Add(1)
		go func(index int) {
			defer uploadWG.Done()
			version := fmt.Sprintf("1.0.%d", index)
			if err := env.uploadNoFail(login.CSRF, "game", version,
				makeManifest("game", version)); err != nil {
				uploadErrors <- err
			}
		}(index)
	}
	uploadWG.Wait()
	close(uploadErrors)
	for err := range uploadErrors {
		t.Fatal(err)
	}

	channel := env.publish(login.CSRF, "game", "1.0.0")
	if channel.Version != "1.0.0" {
		t.Fatalf("channel version = %q", channel.Version)
	}
	tokenResponse := env.request(http.MethodPost, "/api/v1/auth/device-token",
		strings.NewReader(`{"player_id":"p1","package":"game","platform":"windows"}`),
		map[string]string{"Content-Type": "application/json"})
	if tokenResponse.StatusCode != http.StatusOK {
		t.Fatalf("token status %d", tokenResponse.StatusCode)
	}
	token := decode[struct {
		Token string `json:"token"`
	}](t, tokenResponse)
	signedResponse := env.request(http.MethodGet,
		"/api/v1/client/url?path="+url.QueryEscape("/files/game/1.0.0/game_1.0.0.bytes"), nil,
		map[string]string{"Authorization": "Bearer " + token.Token})
	if signedResponse.StatusCode != http.StatusOK {
		t.Fatalf("signed URL status %d", signedResponse.StatusCode)
	}
	var signed signedURLResponse
	signed = decode[signedURLResponse](t, signedResponse)
	signed.URL = strings.Replace(signed.URL, "http://example.test", env.http.URL, 1)
	manifest := makeManifest("game", "1.0.0")

	const workers = 8
	const iterations = 25
	var failures atomic.Int32
	var readerWG sync.WaitGroup
	start := make(chan struct{})
	for worker := 0; worker < workers; worker++ {
		readerWG.Add(1)
		go func(worker int) {
			defer readerWG.Done()
			<-start
			for i := 0; i < iterations; i++ {
				download, err := http.Get(signed.URL)
				if err != nil {
					failures.Add(1)
					continue
				}
				downloaded, readErr := io.ReadAll(download.Body)
				download.Body.Close()
				if readErr != nil || download.StatusCode != http.StatusOK ||
					!bytes.Equal(downloaded, manifest) {
					failures.Add(1)
				}

				configURL := fmt.Sprintf(
					"%s/api/v1/client/config?package=game&channel=prod&platform=windows&player_id=p%d&app_version=1.0.0",
					env.http.URL, worker)
				configResponse, err := http.Get(configURL)
				if err != nil {
					failures.Add(1)
					continue
				}
				_, _ = io.Copy(io.Discard, configResponse.Body)
				configResponse.Body.Close()
				if configResponse.StatusCode != http.StatusOK {
					failures.Add(1)
				}

				unsigned, err := http.Get(env.http.URL + "/files/game/1.0.0/game_1.0.0.bytes")
				if err != nil {
					failures.Add(1)
					continue
				}
				_, _ = io.Copy(io.Discard, unsigned.Body)
				unsigned.Body.Close()
				if unsigned.StatusCode != http.StatusForbidden {
					failures.Add(1)
				}
			}
		}(worker)
	}
	close(start)
	readerWG.Wait()
	if failures.Load() != 0 {
		t.Fatalf("concurrent request failures = %d", failures.Load())
	}
}
func (e *testEnvironment) adminPostNoFail(csrf, path, body string) error {
	request, err := http.NewRequestWithContext(e.t.Context(), http.MethodPost, e.http.URL+path,
		strings.NewReader(body))
	if err != nil {
		return err
	}
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("X-CSRF-Token", csrf)
	response, err := e.client.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	payload, _ := io.ReadAll(response.Body)
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("admin POST %s status %d: %s", path, response.StatusCode, payload)
	}
	return nil
}

func TestConcurrentAdminPublishAndRollback(t *testing.T) {
	env := newTestEnvironment(t)
	login := env.login()
	env.createPackage(login.CSRF, "game")
	env.upload(login.CSRF, "game", "1.0.0", makeManifest("game", "1.0.0"), nil)
	env.upload(login.CSRF, "game", "2.0.0", makeManifest("game", "2.0.0"), nil)
	env.publish(login.CSRF, "game", "1.0.0")

	const admins = 4
	const readers = 4
	const iterations = 10
	var failures atomic.Int32
	errorMessages := make(chan string, admins*iterations)
	var group sync.WaitGroup
	start := make(chan struct{})
	for admin := 0; admin < admins; admin++ {
		group.Add(1)
		go func(admin int) {
			defer group.Done()
			<-start
			for i := 0; i < iterations; i++ {
				var path, body string
				if (admin+i)%2 == 0 {
					path = "/api/v1/admin/packages/game/versions/2.0.0/publish"
					body = `{"channel":"prod"}`
				} else {
					path = "/api/v1/admin/packages/game/rollback"
					body = `{"channel":"prod","version":"1.0.0"}`
				}
				if err := env.adminPostNoFail(login.CSRF, path, body); err != nil {
					failures.Add(1)
					errorMessages <- err.Error()
				}
			}
		}(admin)
	}
	for reader := 0; reader < readers; reader++ {
		group.Add(1)
		go func(reader int) {
			defer group.Done()
			<-start
			for i := 0; i < iterations; i++ {
				configURL := fmt.Sprintf(
					"%s/api/v1/client/config?package=game&channel=prod&platform=windows&player_id=r%d&app_version=1.0.0",
					env.http.URL, reader)
				response, err := http.Get(configURL)
				if err != nil {
					failures.Add(1)
					continue
				}
				_, _ = io.Copy(io.Discard, response.Body)
				response.Body.Close()
				if response.StatusCode != http.StatusOK {
					failures.Add(1)
				}
			}
		}(reader)
	}
	close(start)
	group.Wait()
	if failures.Load() != 0 {
		close(errorMessages)
		for message := range errorMessages {
			t.Log(message)
		}
		t.Fatalf("concurrent admin/config failures = %d", failures.Load())
	}
}
func TestRateLimitAPIKeyAndNotFound(t *testing.T) {
	env := newTestEnvironmentWith(t, func(cfg *config.Config) {
		cfg.RateLimitLogin = 2
	})
	for index := 0; index < 2; index++ {
		login := env.login()
		if login.CSRF == "" {
			t.Fatal("missing CSRF token")
		}
	}
	payload := strings.NewReader(`{"username":"admin","password":"password"}`)
	limited := env.request(http.MethodPost, "/api/v1/admin/login", payload,
		map[string]string{"Content-Type": "application/json"})
	limited.Body.Close()
	if limited.StatusCode != http.StatusTooManyRequests {
		t.Fatalf("rate limit status %d", limited.StatusCode)
	}

	env2 := newTestEnvironment(t)
	login := env2.login()
	keyResponse := env2.request(http.MethodPost, "/api/v1/admin/api-keys",
		strings.NewReader(`{"name":"cli"}`),
		map[string]string{"Content-Type": "application/json", "X-CSRF-Token": login.CSRF})
	if keyResponse.StatusCode != http.StatusCreated {
		t.Fatalf("API key status %d", keyResponse.StatusCode)
	}
	var key struct {
		APIKey string `json:"api_key"`
	}
	key = decode[struct {
		APIKey string `json:"api_key"`
	}](t, keyResponse)
	withKey := env2.request(http.MethodGet, "/api/v1/admin/packages", nil,
		map[string]string{"X-API-Key": key.APIKey})
	withKey.Body.Close()
	if withKey.StatusCode != http.StatusOK {
		t.Fatalf("API key auth status %d", withKey.StatusCode)
	}

	missing := env2.request(http.MethodGet, "/api/v1/client/config?package=missing", nil, nil)
	missing.Body.Close()
	if missing.StatusCode != http.StatusNotFound {
		t.Fatalf("missing package status %d", missing.StatusCode)
	}
	badSignature := env2.request(http.MethodGet,
		"/files/game/1.0.0/game_1.0.0.bytes?expires=9999999999&key_id=test&signature=bad", nil, nil)
	badSignature.Body.Close()
	if badSignature.StatusCode != http.StatusForbidden {
		t.Fatalf("bad signature status %d", badSignature.StatusCode)
	}
}

func TestPackageAndVersionRenameDelete(t *testing.T) {
	env := newTestEnvironmentWith(t, func(cfg *config.Config) {
		cfg.RequireSignedFiles = false
	})
	login := env.login()
	env.createPackage(login.CSRF, "game")
	env.upload(login.CSRF, "game", "1.0.0", makeManifest("game", "1.0.0"),
		map[string][]byte{"one.bundle": makeBundle("one")})
	env.publish(login.CSRF, "game", "1.0.0")

	env.createPackage(login.CSRF, "taken")
	conflict := env.adminJSON(http.MethodPatch, "/api/v1/admin/packages/game", login.CSRF,
		`{"name":"taken"}`)
	conflict.Body.Close()
	if conflict.StatusCode != http.StatusConflict {
		t.Fatalf("package rename conflict status %d", conflict.StatusCode)
	}

	rename := env.adminJSON(http.MethodPatch, "/api/v1/admin/packages/game", login.CSRF,
		`{"name":"game2"}`)
	if rename.StatusCode != http.StatusOK {
		payload, _ := io.ReadAll(rename.Body)
		rename.Body.Close()
		t.Fatalf("package rename status %d: %s", rename.StatusCode, payload)
	}
	pkg := decode[store.Package](t, rename)
	if pkg.Name != "game2" {
		t.Fatalf("renamed package = %q", pkg.Name)
	}
	oldPackage := env.request(http.MethodGet, "/api/v1/admin/packages/game", nil, nil)
	oldPackage.Body.Close()
	if oldPackage.StatusCode != http.StatusNotFound {
		t.Fatalf("old package status %d", oldPackage.StatusCode)
	}
	operator, err := env.store.CreateUser(t.Context(), "operator", "unused", "operator")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := env.store.CreateAPIKey(t.Context(), operator.ID, "operator-cli",
		secure.HashAPIKey("operator-key"), nil); err != nil {
		t.Fatal(err)
	}
	forbidden := env.request(http.MethodPatch, "/api/v1/admin/packages/game2",
		strings.NewReader(`{"name":"operator-rename"}`),
		map[string]string{"Content-Type": "application/json", "X-API-Key": "operator-key"})
	forbidden.Body.Close()
	if forbidden.StatusCode != http.StatusForbidden {
		t.Fatalf("operator rename status %d", forbidden.StatusCode)
	}
	if _, err := env.store.ArtifactByName(t.Context(), "game2", "1.0.0", "game2_1.0.0.bytes"); err != nil {
		t.Fatalf("renamed package manifest artifact: %v", err)
	}
	manifestDownload := env.request(http.MethodGet, "/files/game2/1.0.0/game2_1.0.0.bytes", nil, nil)
	manifestData, _ := io.ReadAll(manifestDownload.Body)
	manifestDownload.Body.Close()
	if manifestDownload.StatusCode != http.StatusOK || !bytes.Equal(manifestData, makeManifest("game", "1.0.0")) {
		t.Fatalf("renamed package manifest status %d", manifestDownload.StatusCode)
	}

	env.upload(login.CSRF, "game2", "2.0.0", makeManifest("game2", "2.0.0"), nil)
	versionConflict := env.adminJSON(http.MethodPatch,
		"/api/v1/admin/packages/game2/versions/1.0.0", login.CSRF, `{"version":"2.0.0"}`)
	versionConflict.Body.Close()
	if versionConflict.StatusCode != http.StatusConflict {
		t.Fatalf("version rename conflict status %d", versionConflict.StatusCode)
	}
	renameVersion := env.adminJSON(http.MethodPatch,
		"/api/v1/admin/packages/game2/versions/1.0.0", login.CSRF, `{"version":"1.1.0"}`)
	if renameVersion.StatusCode != http.StatusOK {
		payload, _ := io.ReadAll(renameVersion.Body)
		renameVersion.Body.Close()
		t.Fatalf("version rename status %d: %s", renameVersion.StatusCode, payload)
	}
	version := decode[store.Version](t, renameVersion)
	if version.Version != "1.1.0" {
		t.Fatalf("renamed version = %q", version.Version)
	}
	renamedManifest := env.request(http.MethodGet, "/files/game2/1.1.0/game2_1.1.0.bytes", nil, nil)
	renamedData, _ := io.ReadAll(renamedManifest.Body)
	renamedManifest.Body.Close()
	if renamedManifest.StatusCode != http.StatusOK || !bytes.Equal(renamedData, makeManifest("game", "1.0.0")) {
		t.Fatalf("renamed version manifest status %d", renamedManifest.StatusCode)
	}
	oldVersion := env.request(http.MethodGet, "/files/game2/1.0.0/game2_1.0.0.bytes", nil, nil)
	oldVersion.Body.Close()
	if oldVersion.StatusCode != http.StatusNotFound {
		t.Fatalf("old version status %d", oldVersion.StatusCode)
	}

	config := decode[clientConfig](t, env.request(http.MethodGet,
		"/api/v1/client/config?package=game2&channel=prod&platform=windows&player_id=p1&app_version=1.1.0",
		nil, nil))
	if config.Version != "1.1.0" {
		t.Fatalf("renamed version config = %+v", config)
	}

	referenced := env.adminJSON(http.MethodDelete,
		"/api/v1/admin/packages/game2/versions/1.1.0?confirm=1.1.0", login.CSRF, "")
	referenced.Body.Close()
	if referenced.StatusCode != http.StatusConflict {
		t.Fatalf("referenced version delete status %d", referenced.StatusCode)
	}
	missingConfirm := env.adminJSON(http.MethodDelete,
		"/api/v1/admin/packages/game2/versions/2.0.0", login.CSRF, "")
	missingConfirm.Body.Close()
	if missingConfirm.StatusCode != http.StatusBadRequest {
		t.Fatalf("missing version confirm status %d", missingConfirm.StatusCode)
	}

	draft, err := env.store.GetVersion(t.Context(), "game2", "2.0.0")
	if err != nil {
		t.Fatal(err)
	}
	draftSHAs := make([]string, 0, len(draft.Artifacts))
	for _, artifact := range draft.Artifacts {
		draftSHAs = append(draftSHAs, artifact.SHA256)
	}
	deletedVersion := env.adminJSON(http.MethodDelete,
		"/api/v1/admin/packages/game2/versions/2.0.0?confirm=2.0.0", login.CSRF, "")
	deletedVersion.Body.Close()
	if deletedVersion.StatusCode != http.StatusNoContent {
		t.Fatalf("delete version status %d", deletedVersion.StatusCode)
	}
	goneVersion := env.request(http.MethodGet, "/api/v1/admin/packages/game2/versions/2.0.0", nil, nil)
	goneVersion.Body.Close()
	if goneVersion.StatusCode != http.StatusNotFound {
		t.Fatalf("deleted version status %d", goneVersion.StatusCode)
	}
	for _, sha := range draftSHAs {
		if env.server.blobs.Has(sha) {
			t.Fatalf("deleted version blob still exists: %s", sha)
		}
	}

	active, err := env.store.GetVersion(t.Context(), "game2", "1.1.0")
	if err != nil {
		t.Fatal(err)
	}
	activeSHAs := make([]string, 0, len(active.Artifacts))
	for _, artifact := range active.Artifacts {
		activeSHAs = append(activeSHAs, artifact.SHA256)
	}
	missingPackageConfirm := env.adminJSON(http.MethodDelete,
		"/api/v1/admin/packages/game2", login.CSRF, "")
	missingPackageConfirm.Body.Close()
	if missingPackageConfirm.StatusCode != http.StatusBadRequest {
		t.Fatalf("missing package confirm status %d", missingPackageConfirm.StatusCode)
	}
	deletedPackage := env.adminJSON(http.MethodDelete,
		"/api/v1/admin/packages/game2?confirm=game2", login.CSRF, "")
	deletedPackage.Body.Close()
	if deletedPackage.StatusCode != http.StatusNoContent {
		t.Fatalf("delete package status %d", deletedPackage.StatusCode)
	}
	gonePackage := env.request(http.MethodGet, "/api/v1/admin/packages/game2", nil, nil)
	gonePackage.Body.Close()
	if gonePackage.StatusCode != http.StatusNotFound {
		t.Fatalf("deleted package status %d", gonePackage.StatusCode)
	}
	for _, sha := range activeSHAs {
		if env.server.blobs.Has(sha) {
			t.Fatalf("deleted package blob still exists: %s", sha)
		}
	}
}

func (e *testEnvironment) adminJSON(method, path, csrf, body string) *http.Response {
	e.t.Helper()
	var reader io.Reader
	if body != "" {
		reader = strings.NewReader(body)
	}
	headers := map[string]string{"X-CSRF-Token": csrf}
	if body != "" {
		headers["Content-Type"] = "application/json"
	}
	return e.request(method, path, reader, headers)
}
