package main

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestMutationCommands(t *testing.T) {
	type expected struct {
		method string
		path   string
		field  string
		value  string
	}
	cases := []struct {
		name string
		run  func() error
		want expected
	}{
		{
			name: "rename package",
			run:  func() error { return runRenamePackage([]string{"--package", "game", "--name", "game-next"}) },
			want: expected{method: http.MethodPatch, path: "/api/v1/admin/packages/game", field: "name", value: "game-next"},
		},
		{
			name: "delete package",
			run:  func() error { return runDeletePackage([]string{"--package", "game", "--confirm", "game"}) },
			want: expected{method: http.MethodDelete, path: "/api/v1/admin/packages/game?confirm=game"},
		},
		{
			name: "rename version",
			run: func() error {
				return runRenameVersion([]string{"--package", "game", "--version", "1.0.0", "--name", "1.0.1"})
			},
			want: expected{method: http.MethodPatch, path: "/api/v1/admin/packages/game/versions/1.0.0", field: "version", value: "1.0.1"},
		},
		{
			name: "delete version",
			run: func() error {
				return runDeleteVersion([]string{"--package", "game", "--version", "1.0.0", "--confirm", "1.0.0"})
			},
			want: expected{method: http.MethodDelete, path: "/api/v1/admin/packages/game/versions/1.0.0?confirm=1.0.0"},
		},
		{
			name: "update gray rule",
			run: func() error {
				return runGrayUpdate([]string{"--package", "game", "--id", "rule-1", "--version", "1.2.0", "--percentage", "20"})
			},
			want: expected{method: http.MethodPut, path: "/api/v1/admin/packages/game/gray-rules/rule-1", field: "percentage", value: "20"},
		},
		{
			name: "delete gray rule",
			run:  func() error { return runGrayDelete([]string{"--package", "game", "--id", "rule-1"}) },
			want: expected{method: http.MethodDelete, path: "/api/v1/admin/packages/game/gray-rules/rule-1"},
		},
	}
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		payload, _ := io.ReadAll(request.Body)
		index := len(cases) - 1
		for i, item := range cases {
			if item.want.method == request.Method && item.want.path == request.URL.RequestURI() {
				index = i
				break
			}
		}
		item := cases[index]
		if request.Method != item.want.method || request.URL.RequestURI() != item.want.path {
			t.Errorf("unexpected request %s %s", request.Method, request.URL.RequestURI())
			writer.WriteHeader(http.StatusBadRequest)
			return
		}
		if item.want.field != "" {
			var body map[string]any
			if err := json.Unmarshal(payload, &body); err != nil {
				t.Errorf("%s body is invalid: %v", item.name, err)
			}
			if value := body[item.want.field]; value == nil || strings.TrimSpace(fmt.Sprint(value)) != item.want.value {
				t.Errorf("%s body %s = %v", item.name, item.want.field, value)
			}
		}
		if request.Method == http.MethodDelete {
			writer.WriteHeader(http.StatusNoContent)
			return
		}
		writer.Header().Set("Content-Type", "application/json")
		_, _ = writer.Write([]byte(`{}`))
	}))
	defer server.Close()
	t.Setenv("PARCEL_SERVER_URL", server.URL)
	t.Setenv("PARCEL_API_KEY", "test-key")
	for _, item := range cases {
		if err := item.run(); err != nil {
			t.Fatalf("%s: %v", item.name, err)
		}
	}
}
