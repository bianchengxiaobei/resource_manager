package api

import (
	"bytes"
	"context"
	"errors"
	"mime"
	"net/http"
	"path"
	"strconv"
	"strings"
	"time"

	"parcel/server/internal/store"
)

type resolvedFile struct {
	Package  string
	Version  string
	Name     string
	Text     string
	Artifact *store.Artifact
}

func (s *Server) handleFile(writer http.ResponseWriter, request *http.Request) {
	query := request.URL.Query()
	hasSignature := query.Get("signature") != "" || query.Get("expires") != "" || query.Get("key_id") != ""
	if hasSignature {
		expires, err := strconv.ParseInt(query.Get("expires"), 10, 64)
		if err != nil || query.Get("signature") == "" || query.Get("key_id") == "" {
			s.metrics.Inc("parcel_download_failures_total", map[string]string{"reason": "signature"})
			writeError(writer, request, http.StatusForbidden, "forbidden", "signed URL is invalid")
			return
		}
		if err := s.signer.Verify(request.URL.Path, expires, query.Get("key_id"), query.Get("signature")); err != nil {
			s.metrics.Inc("parcel_download_failures_total", map[string]string{"reason": "signature"})
			writeError(writer, request, http.StatusForbidden, "forbidden", "signed URL is invalid or expired")
			return
		}
	} else if s.cfg.RequireSignedFiles {
		s.metrics.Inc("parcel_download_failures_total", map[string]string{"reason": "unsigned"})
		writeError(writer, request, http.StatusForbidden, "forbidden", "signed URL is required")
		return
	}

	resolved, err := s.resolveFilePath(request.Context(), request.URL.Path)
	if err != nil {
		if errors.Is(err, store.ErrInvalid) {
			writeError(writer, request, http.StatusBadRequest, "invalid_request", "file path is invalid")
			return
		}
		s.metrics.Inc("parcel_download_failures_total", map[string]string{"reason": "not_found"})
		writeError(writer, request, http.StatusNotFound, "not_found", "file not found")
		return
	}
	if resolved.Text != "" {
		value := []byte(resolved.Text)
		writer.Header().Set("Content-Type", "text/plain; charset=utf-8")
		writer.Header().Set("Cache-Control", "no-cache")
		writer.Header().Set("ETag", `"`+resolved.Version+`"`)
		http.ServeContent(writer, request, resolved.Name, time.Time{}, bytes.NewReader(value))
		return
	}
	artifact := resolved.Artifact
	file, err := s.blobs.Open(artifact.SHA256)
	if err != nil {
		writeError(writer, request, http.StatusNotFound, "not_found", "file not found")
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		writeError(writer, request, http.StatusInternalServerError, "internal", "file is unavailable")
		return
	}
	contentType := mime.TypeByExtension(path.Ext(resolved.Name))
	if contentType == "" {
		contentType = "application/octet-stream"
	}
	writer.Header().Set("Content-Type", contentType)
	writer.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
	writer.Header().Set("ETag", `"`+artifact.SHA256+`"`)
	s.metrics.Inc("parcel_downloads_total", map[string]string{
		"package": resolved.Package,
		"version": resolved.Version,
		"kind":    artifact.Kind,
	})
	s.metrics.Add("parcel_download_bytes_total", map[string]string{
		"package": resolved.Package,
		"kind":    artifact.Kind,
	}, float64(artifact.Size))
	http.ServeContent(writer, request, resolved.Name, info.ModTime(), file)
}

func (s *Server) resolveFilePath(ctx context.Context, requestPath string) (resolvedFile, error) {
	relative := strings.TrimPrefix(requestPath, "/files/")
	if relative == requestPath || relative == "" || strings.Contains(relative, "\\") ||
		strings.Contains(relative, "\x00") || strings.Contains(relative, "..") {
		return resolvedFile{}, store.ErrInvalid
	}
	cleaned := path.Clean(relative)
	if cleaned != relative || strings.HasPrefix(cleaned, "/") || strings.HasPrefix(cleaned, "../") {
		return resolvedFile{}, store.ErrInvalid
	}
	parts := strings.Split(cleaned, "/")
	if len(parts) < 3 {
		return resolvedFile{}, store.ErrInvalid
	}
	packageName := parts[0]
	version := parts[1]
	name := strings.Join(parts[2:], "/")
	if !safePackage.MatchString(packageName) || !safeVersion.MatchString(version) {
		return resolvedFile{}, store.ErrInvalid
	}
	if strings.HasPrefix(name, "bundles/") {
		if strings.Count(name, "/") != 1 || !safeBundle.MatchString(path.Base(name)) {
			return resolvedFile{}, store.ErrInvalid
		}
	} else if name != packageName+".version" &&
		name != packageName+"_"+version+".bytes" &&
		name != packageName+"_"+version+".hash" {
		return resolvedFile{}, store.ErrInvalid
	}
	if name == packageName+".version" {
		value, err := s.store.GetVersion(ctx, packageName, version)
		if err != nil {
			return resolvedFile{}, err
		}
		return resolvedFile{Package: packageName, Version: value.Version, Name: name, Text: value.Version + "\n"}, nil
	}
	artifact, err := s.store.ArtifactByName(ctx, packageName, version, name)
	if err != nil {
		return resolvedFile{}, err
	}
	return resolvedFile{Package: packageName, Version: version, Name: name, Artifact: &artifact}, nil
}
