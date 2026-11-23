package api

import (
	"net/http"
	"strings"

	"parcel/server/internal/store"
)

type grayRuleRequest struct {
	Version       string   `json:"version"`
	Percentage    int      `json:"percentage"`
	Whitelist     []string `json:"whitelist"`
	Platforms     []string `json:"platforms"`
	MinAppVersion string   `json:"min_app_version"`
	Priority      int      `json:"priority"`
	Enabled       *bool    `json:"enabled"`
}

func (s *Server) handleListGrayRules(writer http.ResponseWriter, request *http.Request, _ *adminIdentity) {
	values, err := s.store.ListGrayRules(request.Context(), request.PathValue("pkg"))
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	writeJSON(writer, http.StatusOK, map[string]any{"items": values})
}

func (s *Server) handleCreateGrayRule(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	var body grayRuleRequest
	if !decodeJSON(writer, request, &body, 256<<10) {
		return
	}
	input, err := s.grayInput(body, true)
	if err != nil {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", err.Error())
		return
	}
	value, err := s.store.CreateGrayRule(request.Context(), request.PathValue("pkg"), input)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	s.audit(request, identity, "gray.create", request.PathValue("pkg")+"/"+value.ID, value.Version)
	writeJSON(writer, http.StatusCreated, value)
}

func (s *Server) handleUpdateGrayRule(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	var body grayRuleRequest
	if !decodeJSON(writer, request, &body, 256<<10) {
		return
	}
	if body.Enabled == nil {
		existing, err := s.store.GrayRuleByID(request.Context(), request.PathValue("pkg"), request.PathValue("id"))
		if err != nil {
			mapStoreError(writer, request, err)
			return
		}
		enabled := existing.Enabled
		body.Enabled = &enabled
	}
	input, err := s.grayInput(body, false)
	if err != nil {
		writeError(writer, request, http.StatusBadRequest, "invalid_request", err.Error())
		return
	}
	value, err := s.store.UpdateGrayRule(request.Context(), request.PathValue("pkg"), request.PathValue("id"), input)
	if err != nil {
		mapStoreError(writer, request, err)
		return
	}
	s.audit(request, identity, "gray.update", request.PathValue("pkg")+"/"+value.ID, value.Version)
	writeJSON(writer, http.StatusOK, value)
}

func (s *Server) handleDeleteGrayRule(writer http.ResponseWriter, request *http.Request, identity *adminIdentity) {
	if err := s.store.DeleteGrayRule(request.Context(), request.PathValue("pkg"), request.PathValue("id")); err != nil {
		mapStoreError(writer, request, err)
		return
	}
	s.audit(request, identity, "gray.delete", request.PathValue("pkg")+"/"+request.PathValue("id"), "")
	writer.WriteHeader(http.StatusNoContent)
}

func (s *Server) grayInput(body grayRuleRequest, create bool) (store.GrayRuleInput, error) {
	body.Version = strings.TrimSpace(body.Version)
	body.MinAppVersion = strings.TrimSpace(body.MinAppVersion)
	if !safeVersion.MatchString(body.Version) {
		return store.GrayRuleInput{}, errInvalid("invalid version")
	}
	if body.Percentage < 0 || body.Percentage > 100 {
		return store.GrayRuleInput{}, errInvalid("percentage must be between 0 and 100")
	}
	if body.Priority < -1000 || body.Priority > 1000 {
		return store.GrayRuleInput{}, errInvalid("priority is out of range")
	}
	if len(body.MinAppVersion) > 64 {
		return store.GrayRuleInput{}, errInvalid("min_app_version is too long")
	}
	enabled := true
	if body.Enabled != nil {
		enabled = *body.Enabled
	} else if !create {
		enabled = false
	}
	return store.GrayRuleInput{
		Version:       body.Version,
		Percentage:    body.Percentage,
		Whitelist:     body.Whitelist,
		Platforms:     body.Platforms,
		MinAppVersion: body.MinAppVersion,
		Priority:      body.Priority,
		Enabled:       enabled,
	}, nil
}

type invalidError string

func (e invalidError) Error() string { return string(e) }

func errInvalid(message string) error { return invalidError(message) }
