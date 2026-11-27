package api

import (
	"context"
	"net/http"
	"strings"
)

type renamePackageRequest struct {
	Name string `json:"name"`
}

type renameVersionRequest struct {
	Version string `json:"version"`
}

func (s *Server) handleRenamePackage(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	if !s.requireAdminRole(writer, request, identity) {
		return
	}
	var body renamePackageRequest
	if !decodeJSON(writer, request, &body, 64<<10) {
		return
	}
	body.Name = strings.TrimSpace(body.Name)
	if !safePackage.MatchString(body.Name) {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid package name")
		return
	}
	oldName := request.PathValue("pkg")
	if !safePackage.MatchString(oldName) {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid package name")
		return
	}
	value, err := s.store.RenamePackage(request.Context(), oldName, body.Name)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	s.audit(request, identity, "package.rename", oldName, value.Name)
	writeJSON(writer, http.StatusOK, value)
}

func (s *Server) handleDeletePackage(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	if !s.requireAdminRole(writer, request, identity) {
		return
	}
	name := request.PathValue("pkg")
	if !safePackage.MatchString(name) {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid package name")
		return
	}
	if request.URL.Query().Get("confirm") != name {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "confirm query parameter must match package name")
		return
	}
	shas, err := s.store.DeletePackage(request.Context(), name)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	s.cleanupBlobs(request.Context(), shas)
	s.audit(request, identity, "package.delete", name, "")
	writer.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleRenameVersion(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	if !s.requireAdminRole(writer, request, identity) {
		return
	}
	var body renameVersionRequest
	if !decodeJSON(writer, request, &body, 64<<10) {
		return
	}
	body.Version = strings.TrimSpace(body.Version)
	if !safeVersion.MatchString(body.Version) {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid version")
		return
	}
	packageName := request.PathValue("pkg")
	oldVersion := request.PathValue("version")
	if !safePackage.MatchString(packageName) || !safeVersion.MatchString(oldVersion) {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid package or version")
		return
	}
	value, err := s.store.RenameVersion(request.Context(), packageName, oldVersion, body.Version)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	s.audit(request, identity, "version.rename", packageName+"/"+oldVersion, value.Version)
	writeJSON(writer, http.StatusOK, value)
}

func (s *Server) handleDeleteVersion(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	if !s.requireAdminRole(writer, request, identity) {
		return
	}
	packageName := request.PathValue("pkg")
	version := request.PathValue("version")
	if !safePackage.MatchString(packageName) || !safeVersion.MatchString(version) {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "invalid package or version")
		return
	}
	if request.URL.Query().Get("confirm") != version {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", "confirm query parameter must match version")
		return
	}
	shas, err := s.store.DeleteVersion(request.Context(), packageName, version)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	s.cleanupBlobs(request.Context(), shas)
	s.audit(request, identity, "version.delete", packageName+"/"+version, "")
	writer.WriteHeader(http.StatusNoContent)
}

func (s *Server) requireAdminRole(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) bool {
	if identity.User.Role != "admin" {
		writeError(writer, request, http.StatusForbidden, "forbidden", "admin role is required")
		return false
	}
	return true
}

func (s *Server) cleanupBlobs(ctx context.Context, shas []string) {
	if s.blobs == nil || len(shas) == 0 {
		return
	}
	seen := make(map[string]struct{}, len(shas))
	for _, sha := range shas {
		if _, exists := seen[sha]; exists {
			continue
		}
		seen[sha] = struct{}{}
		used, err := s.store.ArtifactSHAExists(ctx, sha)
		if err != nil {
			s.logger.Error("blob reference check failed", "sha256", sha, "error", err)
			continue
		}
		if used {
			continue
		}
		if err := s.blobs.Remove(sha); err != nil {
			s.logger.Error("blob cleanup failed", "sha256", sha, "error", err)
		}
	}
}
