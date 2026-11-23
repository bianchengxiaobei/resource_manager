package api

import (
	"io"
	"mime"
	"net/http"
	"path"
	"strings"
)

func (s *Server) handleStatic() http.Handler {
	return http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if s.web == nil || (request.Method != http.MethodGet && request.Method != http.MethodHead) {
			writeError(writer, request, http.StatusNotFound, "not_found", "not found")
			return
		}
		relative := strings.TrimPrefix(path.Clean("/"+request.URL.Path), "/")
		if strings.HasPrefix(relative, "api/") || relative == "healthz" || relative == "metrics" {
			writeError(writer, request, http.StatusNotFound, "not_found", "not found")
			return
		}
		if relative == "" {
			relative = "index.html"
		}
		file, err := s.web.Open(relative)
		if err != nil {
			relative = "index.html"
			file, err = s.web.Open(relative)
			if err != nil {
				writeError(writer, request, http.StatusNotFound, "not_found", "not found")
				return
			}
		}
		defer file.Close()
		info, err := file.Stat()
		if err != nil || info.IsDir() {
			writeError(writer, request, http.StatusNotFound, "not_found", "not found")
			return
		}
		seeker, ok := file.(io.ReadSeeker)
		if !ok {
			writeError(writer, request, http.StatusInternalServerError, "internal", "static asset is not seekable")
			return
		}
		contentType := mime.TypeByExtension(path.Ext(relative))
		if contentType == "" {
			contentType = "application/octet-stream"
		}
		writer.Header().Set("Content-Type", contentType)
		if relative == "index.html" || !strings.HasPrefix(relative, "assets/") {
			writer.Header().Set("Cache-Control", "no-cache")
		} else {
			writer.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
		}
		http.ServeContent(writer, request, relative, info.ModTime(), seeker)
	})
}
