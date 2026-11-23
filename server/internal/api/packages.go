package api

import (
	"crypto/sha256"
	"crypto/subtle"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"io"
	"net/http"
	"path"
	"path/filepath"
	"regexp"
	"strings"

	"parcel/server/internal/blob"
	"parcel/server/internal/store"
)

const manifestHeaderSize = 160

var (
	safePackage = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$`)
	safeVersion = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$`)
	safeBundle  = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,255}\.bundle$`)
)

type createPackageRequest struct {
	Name string `json:"name"`
}

type publishRequest struct {
	Channel string `json:"channel"`
}

type rollbackRequest struct {
	Channel string `json:"channel"`
	Version string `json:"version"`
}

type setChannelRequest struct {
	Version string `json:"version"`
}

type uploadPart struct {
	filename string
	sha256   string
	size     int64
}

func (s *Server) handleListPackages(writer http.ResponseWriter, request *http.Request, _ *adminIdentity) {
	values, err := s.store.ListPackages(request.Context())
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	writeJSON(writer, http.StatusOK, map[string]any{"items": values})
}

func (s *Server) handleCreatePackage(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	if identity.User.Role != "admin" {
		writeError(writer, request, http.StatusForbidden, "forbidden", "admin role is required")
		return
	}
	var body createPackageRequest
	if !decodeJSON(writer, request, &body, 64<<10) {
		return
	}
	body.Name = strings.TrimSpace(body.Name)
	if !safePackage.MatchString(body.Name) {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid package name")
		return
	}
	value, err := s.store.CreatePackage(request.Context(), body.Name, identity.User.ID)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	s.audit(request, identity, "package.create", value.Name, "")
	writeJSON(writer, http.StatusCreated, value)
}

func (s *Server) handleGetPackage(writer http.ResponseWriter, request *http.Request, _ *adminIdentity) {
	pkg, err := s.store.PackageByName(request.Context(), request.PathValue("pkg"))
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	channels, err := s.store.ListChannels(request.Context(), pkg.Name)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	writeJSON(writer, http.StatusOK, map[string]any{"package": pkg, "channels": channels})
}

func (s *Server) handleListVersions(writer http.ResponseWriter, request *http.Request, _ *adminIdentity) {
	values, err := s.store.ListVersions(request.Context(), request.PathValue("pkg"))
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	writeJSON(writer, http.StatusOK, map[string]any{"items": values})
}

func (s *Server) handleGetVersion(writer http.ResponseWriter, request *http.Request, _ *adminIdentity) {
	value, err := s.store.GetVersion(request.Context(), request.PathValue("pkg"), request.PathValue("version"))
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	writeJSON(writer, http.StatusOK, value)
}

func (s *Server) handleUploadVersion(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	packageName := request.PathValue("pkg")
	if !safePackage.MatchString(packageName) {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid package name")
		return
	}
	if _, err := s.store.PackageByName(request.Context(), packageName); err != nil {
		mapStoreError(writer, request, err)
		return
	}
	request.Body = http.MaxBytesReader(writer, request.Body, s.cfg.MaxUploadBytes+(1<<20))
	reader, err := request.MultipartReader()
	if err != nil {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "multipart body is required")
		return
	}

	var version, note, suppliedHash string
	var manifestBytes []byte
	var bundles []uploadPart
	seenBundles := make(map[string]struct{})
	var total int64
	for {
		part, err := reader.NextPart()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			if strings.Contains(err.Error(), "request body too large") {
				writeError(writer, request, http.StatusRequestEntityTooLarge, "payload_too_large", "upload is too large")
				return
			}
			writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid multipart body")
			return
		}
		field := part.FormName()
		filename := part.FileName()
		if filename == "" {
			value, err := readField(part, 16<<10)
			if err != nil {
				writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid multipart field")
				return
			}
			switch field {
			case "version":
				version = strings.TrimSpace(value)
			case "note":
				note = value
			case "manifest_hash":
				suppliedHash = strings.TrimSpace(value)
			}
			continue
		}
		switch field {
		case "manifest":
			if len(manifestBytes) != 0 {
				writeError(writer, request, http.StatusBadRequest, "invalid_request", "only one manifest is allowed")
				return
			}
			value, err := io.ReadAll(io.LimitReader(part, s.cfg.MaxManifestBytes+1))
			if err != nil {
				writeError(writer, request, http.StatusBadRequest, "invalid_request", "could not read manifest")
				return
			}
			if int64(len(value)) > s.cfg.MaxManifestBytes {
				writeError(writer, request, http.StatusRequestEntityTooLarge, "payload_too_large", "manifest is too large")
				return
			}
			manifestBytes = value
			total += int64(len(value))
		case "bundles":
			name := filepath.Base(filename)
			if name != filename || !safeBundle.MatchString(name) {
				writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid bundle file name")
				return
			}
			if _, exists := seenBundles[name]; exists {
				writeError(writer, request, http.StatusBadRequest, "invalid_request", "duplicate bundle file name")
				return
			}
			seenBundles[name] = struct{}{}
			info, err := s.blobs.Save(part, s.cfg.MaxBundleBytes)
			if err != nil {
				if errors.Is(err, blob.ErrTooLarge) {
					writeError(writer, request, http.StatusRequestEntityTooLarge, "payload_too_large", "bundle is too large")
					return
				}
				writeError(writer, request, http.StatusInternalServerError, "internal", "could not store bundle")
				return
			}
			if err := s.validateStoredBundle(info.SHA256); err != nil {
				writeError(writer, request, http.StatusBadRequest, "invalid_bundle", err.Error())
				return
			}
			total += info.Size
			bundles = append(bundles, uploadPart{filename: name, sha256: info.SHA256, size: info.Size})
		default:
			writeError(writer, request, http.StatusBadRequest, "invalid_request", "unknown multipart file field")
			return
		}
		if total > s.cfg.MaxUploadBytes {
			writeError(writer, request, http.StatusRequestEntityTooLarge, "payload_too_large", "upload is too large")
			return
		}
	}

	version = strings.TrimSpace(version)
	if !safeVersion.MatchString(version) {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid version")
		return
	}
	if len(manifestBytes) == 0 {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "manifest file is required")
		return
	}
	if len(note) > 4096 {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "note is too long")
		return
	}
	manifestHash, err := validateManifest(manifestBytes)
	if err != nil {
		writeError(writer, request, http.StatusBadRequest, "invalid_manifest", err.Error())
		return
	}
	if suppliedHash != "" {
		suppliedHash = strings.TrimPrefix(strings.ToLower(strings.TrimSpace(suppliedHash)), "sha256:")
		if len(suppliedHash) != 64 || suppliedHash != manifestHash {
			writeError(writer, request, http.StatusBadRequest, "invalid_manifest", "manifest hash does not match")
			return
		}
	}
	for _, bundle := range bundles {
		if err := validateBundleFile(bundle.filename); err != nil {
			writeError(writer, request, http.StatusBadRequest, "invalid_bundle", err.Error())
			return
		}
	}
	manifestInfo, err := s.blobs.SaveBytes(manifestBytes, s.cfg.MaxManifestBytes)
	if err != nil {
		writeError(writer, request, http.StatusInternalServerError, "internal", "could not store manifest")
		return
	}
	hashInfo, err := s.blobs.SaveBytes([]byte(manifestHash+"\n"), 1024)
	if err != nil {
		writeError(writer, request, http.StatusInternalServerError, "internal", "could not store manifest hash")
		return
	}
	artifacts := []store.ArtifactInput{
		{Kind: "manifest", Name: packageName + "_" + version + ".bytes", SHA256: manifestInfo.SHA256, Size: manifestInfo.Size},
		{Kind: "manifest_hash", Name: packageName + "_" + version + ".hash", SHA256: hashInfo.SHA256, Size: hashInfo.Size},
	}
	for _, bundle := range bundles {
		artifacts = append(artifacts, store.ArtifactInput{
			Kind:   "bundle",
			Name:   "bundles/" + bundle.filename,
			SHA256: bundle.sha256,
			Size:   bundle.size,
		})
	}
	value, err := s.store.CreateVersion(request.Context(), packageName, version, manifestHash, note, artifacts)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	s.metrics.Inc("parcel_uploads_total", map[string]string{"package": packageName})
	s.metrics.Add("parcel_upload_bytes_total", map[string]string{"package": packageName}, float64(total))
	s.audit(request, identity, "version.upload", packageName+"/"+version, manifestHash)
	writeJSON(writer, http.StatusCreated, value)
}

func (s *Server) handlePublish(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	var body publishRequest
	if request.ContentLength != 0 && !decodeJSON(writer, request, &body, 64<<10) {
		return
	}
	if body.Channel == "" {
		body.Channel = "prod"
	}
	if !safeComponent(body.Channel, 64) {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid channel")
		return
	}
	value, err := s.store.Publish(request.Context(), request.PathValue("pkg"), request.PathValue("version"), body.Channel)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	s.audit(request, identity, "version.publish", request.PathValue("pkg")+"/"+body.Channel,
		request.PathValue("version"))
	writeJSON(writer, http.StatusOK, value)
}

func (s *Server) handleRollback(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	var body rollbackRequest
	if request.ContentLength != 0 && !decodeJSON(writer, request, &body, 64<<10) {
		return
	}
	if body.Channel == "" {
		body.Channel = "prod"
	}
	if !safeComponent(body.Channel, 64) || (body.Version != "" && !safeVersion.MatchString(body.Version)) {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid rollback parameters")
		return
	}
	value, err := s.store.Rollback(request.Context(), request.PathValue("pkg"), body.Channel, body.Version)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	s.audit(request, identity, "version.rollback", request.PathValue("pkg")+"/"+body.Channel, value.Version)
	writeJSON(writer, http.StatusOK, value)
}

func (s *Server) handleListChannels(writer http.ResponseWriter, request *http.Request, _ *adminIdentity) {
	values, err := s.store.ListChannels(request.Context(), request.PathValue("pkg"))
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	writeJSON(writer, http.StatusOK, map[string]any{"items": values})
}

func (s *Server) handleSetChannel(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	var body setChannelRequest
	if !decodeJSON(writer, request, &body, 64<<10) {
		return
	}
	channel := request.PathValue("channel")
	if !safeComponent(channel, 64) || !safeVersion.MatchString(body.Version) {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid channel or version")
		return
	}
	value, err := s.store.SetChannel(request.Context(), request.PathValue("pkg"), channel, body.Version)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	s.audit(request, identity, "channel.set", request.PathValue("pkg")+"/"+channel, body.Version)
	writeJSON(writer, http.StatusOK, value)
}

func readField(reader io.Reader, limit int64) (string, error) {
	value, err := io.ReadAll(io.LimitReader(reader, limit+1))
	if err != nil {
		return "", err
	}
	if int64(len(value)) > limit {
		return "", errors.New("field is too large")
	}
	return string(value), nil
}

func validateManifest(value []byte) (string, error) {
	if len(value) < manifestHeaderSize {
		return "", errors.New("manifest is truncated")
	}
	if string(value[:4]) != "PRCM" {
		return "", errors.New("manifest magic is invalid")
	}
	if binary.LittleEndian.Uint16(value[4:6]) != 1 {
		return "", errors.New("manifest version is unsupported")
	}
	if binary.LittleEndian.Uint16(value[6:8]) != manifestHeaderSize {
		return "", errors.New("manifest header size is invalid")
	}
	actual := sha256Bytes(value[manifestHeaderSize:])
	if subtle.ConstantTimeCompare(value[96:128], actual[:]) != 1 {
		return "", errors.New("manifest content hash does not match")
	}
	return hex.EncodeToString(actual[:]), nil
}

func sha256Bytes(value []byte) [32]byte {
	return sha256.Sum256(value)
}

func (s *Server) validateStoredBundle(sha string) error {
	file, err := s.blobs.Open(sha)
	if err != nil {
		return errors.New("bundle is unavailable")
	}
	defer file.Close()
	header := make([]byte, 128)
	if _, err := io.ReadFull(file, header); err != nil {
		return errors.New("bundle is truncated")
	}
	if string(header[:4]) != "PRCL" ||
		binary.LittleEndian.Uint16(header[4:6]) != 1 ||
		binary.LittleEndian.Uint16(header[6:8]) != 128 {
		return errors.New("bundle header is invalid")
	}
	return nil
}

func validateBundleFile(name string) error {
	if !safeBundle.MatchString(name) || path.Base(name) != name {
		return errors.New("invalid bundle file name")
	}
	return nil
}

func safeComponent(value string, limit int) bool {
	return value != "" && len(value) <= limit && !strings.Contains(value, "..") &&
		!strings.ContainsAny(value, `/\`) && !strings.Contains(value, "\x00")
}
