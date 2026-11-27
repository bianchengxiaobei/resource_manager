package api

import (
	"os"
	"path/filepath"
	"reflect"
	"regexp"
	"sort"
	"strings"
	"testing"

	"parcel/server/internal/metrics"
	"parcel/server/internal/store"
)

func TestOpenAPIRoutesMatchServer(t *testing.T) {
	documented, err := readOpenAPIRoutes(filepath.Join("..", "..", "..", "api", "openapi.yaml"))
	if err != nil {
		t.Fatal(err)
	}
	registered, err := readRegisteredRoutes("server.go")
	if err != nil {
		t.Fatal(err)
	}

	documentedRoutes := sortedKeys(documented)
	registeredRoutes := sortedKeys(registered)
	for _, route := range registeredRoutes {
		if _, ok := documented[route]; !ok {
			t.Errorf("server route %q is not documented in api/openapi.yaml", route)
		}
	}
	for _, route := range documentedRoutes {
		if _, ok := registered[route]; !ok {
			t.Errorf("OpenAPI route %q is not registered by the server", route)
		}
	}
}

func TestOpenAPISchemaPropertiesMatchGoTypes(t *testing.T) {
	schemas, err := readOpenAPISchemaProperties(filepath.Join("..", "..", "..", "api", "openapi.yaml"))
	if err != nil {
		t.Fatal(err)
	}
	cases := map[string]any{
		"Error":                errorBody{},
		"LoginRequest":         loginRequest{},
		"DeviceTokenRequest":   deviceTokenRequest{},
		"CreatePackageRequest": createPackageRequest{},
		"RenamePackageRequest": renamePackageRequest{},
		"RenameVersionRequest": renameVersionRequest{},
		"Package":              store.Package{},
		"Artifact":             store.Artifact{},
		"Version":              store.Version{},
		"Channel":              store.Channel{},
		"PublishRequest":       publishRequest{},
		"RollbackRequest":      rollbackRequest{},
		"SetChannelRequest":    setChannelRequest{},
		"GrayRuleRequest":      grayRuleRequest{},
		"CreateAPIKeyRequest":  createAPIKeyRequest{},
		"ClientRule":           clientRule{},
		"ClientConfig":         clientConfig{},
		"ClientVersion":        clientVersionResponse{},
		"SignedURL":            signedURLResponse{},
		"AuditEntry":           store.AuditEntry{},
		"AuditPage":            pagination{},
		"MetricsSnapshot":      metrics.Snapshot{},
	}
	for name, value := range cases {
		documented := sortedSetKeys(schemas[name])
		actual := jsonFieldNames(value)
		if !reflect.DeepEqual(documented, actual) {
			t.Errorf("%s properties mismatch: OpenAPI=%v Go=%v", name, documented, actual)
		}
	}
}

func readOpenAPISchemaProperties(path string) (map[string]map[string]struct{}, error) {
	content, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	schemas := make(map[string]map[string]struct{})
	inSchemas := false
	schemaName := ""
	inProperties := false
	for _, line := range strings.Split(string(content), "\n") {
		if line == "  schemas:" {
			inSchemas = true
			continue
		}
		if !inSchemas {
			continue
		}
		if strings.HasPrefix(line, "    ") && !strings.HasPrefix(line, "      ") &&
			strings.HasSuffix(strings.TrimSpace(line), ":") {
			schemaName = strings.TrimSuffix(strings.TrimSpace(line), ":")
			schemas[schemaName] = make(map[string]struct{})
			inProperties = false
			continue
		}
		if schemaName == "" {
			continue
		}
		if strings.HasPrefix(line, "      properties:") {
			inProperties = true
			continue
		}
		if !inProperties {
			continue
		}
		if strings.HasPrefix(line, "        ") && !strings.HasPrefix(line, "          ") &&
			strings.HasSuffix(strings.TrimSpace(line), ":") {
			name := strings.TrimSuffix(strings.TrimSpace(line), ":")
			schemas[schemaName][name] = struct{}{}
		}
	}
	return schemas, nil
}

func jsonFieldNames(value any) []string {
	typ := reflect.TypeOf(value)
	names := make([]string, 0, typ.NumField())
	for index := 0; index < typ.NumField(); index++ {
		tag := typ.Field(index).Tag.Get("json")
		name := strings.Split(tag, ",")[0]
		if name == "" || name == "-" {
			continue
		}
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

func sortedSetKeys(values map[string]struct{}) []string {
	keys := make([]string, 0, len(values))
	for key := range values {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	return keys
}

func readOpenAPIRoutes(path string) (map[string]struct{}, error) {
	content, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	routes := make(map[string]struct{})
	inPaths := false
	pathName := ""
	for _, line := range strings.Split(string(content), "\n") {
		if strings.HasPrefix(line, "paths:") {
			inPaths = true
			continue
		}
		if !inPaths {
			continue
		}
		if strings.HasPrefix(line, "components:") {
			break
		}
		if strings.HasPrefix(line, "  /") {
			pathName = strings.TrimSuffix(strings.TrimSpace(line), ":")
			continue
		}
		if pathName == "" {
			continue
		}
		switch strings.TrimSpace(line) {
		case "get:", "post:", "put:", "delete:", "patch:":
			method := strings.ToUpper(strings.TrimSuffix(strings.TrimSpace(line), ":"))
			routes[normalizeRoute(method+" "+pathName)] = struct{}{}
		}
	}
	return routes, nil
}

func readRegisteredRoutes(path string) (map[string]struct{}, error) {
	content, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	pattern := regexp.MustCompile(`s\.mux\.(?:HandleFunc|Handle)\("([A-Z]+) ([^"]+)"`)
	routes := make(map[string]struct{})
	for _, match := range pattern.FindAllStringSubmatch(string(content), -1) {
		routes[normalizeRoute(match[1]+" "+match[2])] = struct{}{}
	}
	return routes, nil
}

func normalizeRoute(route string) string {
	return strings.ReplaceAll(route, "{path...}", "{path}")
}

func sortedKeys(values map[string]struct{}) []string {
	keys := make([]string, 0, len(values))
	for key := range values {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	return keys
}
