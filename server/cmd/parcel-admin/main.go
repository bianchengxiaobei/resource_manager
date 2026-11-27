package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"

	"parcel/server/internal/backup"
)

type stringList []string

func (s *stringList) String() string { return strings.Join(*s, ",") }

func (s *stringList) Set(value string) error {
	*s = append(*s, value)
	return nil
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	var err error
	switch os.Args[1] {
	case "upload":
		err = runUpload(os.Args[2:])
	case "publish":
		err = runPublish(os.Args[2:])
	case "rollback":
		err = runRollback(os.Args[2:])
	case "rename-package":
		err = runRenamePackage(os.Args[2:])
	case "delete-package":
		err = runDeletePackage(os.Args[2:])
	case "rename-version":
		err = runRenameVersion(os.Args[2:])
	case "delete-version":
		err = runDeleteVersion(os.Args[2:])
	case "gray":
		err = runGray(os.Args[2:])
	case "gray-update":
		err = runGrayUpdate(os.Args[2:])
	case "gray-delete":
		err = runGrayDelete(os.Args[2:])
	case "backup":
		err = runBackup(os.Args[2:])
	case "restore":
		err = runRestore(os.Args[2:])
	case "help", "-h", "--help":
		usage()
		return
	default:
		err = fmt.Errorf("unknown command %q", os.Args[1])
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "parcel-admin:", err)
		os.Exit(1)
	}
}

func usage() {
	fmt.Print(`Parcel admin CLI

Usage:
  parcel-admin upload --package game --version 1.0.0 --manifest build/manifest.bytes \\
      --bundle build/bundles/foo.bundle [--bundle ...] [--note text]
  parcel-admin publish --package game --version 1.0.0 [--channel prod]
  parcel-admin rollback --package game [--channel prod] [--version 0.9.0]
  parcel-admin rename-package --package game --name game-next
  parcel-admin delete-package --package game --confirm game
  parcel-admin rename-version --package game --version 1.0.0 --name 1.0.1
  parcel-admin delete-version --package game --version 1.0.0 --confirm 1.0.0
  parcel-admin gray --package game --version 1.1.0 --percentage 10 [--whitelist a,b]
  parcel-admin gray-update --package game --id RULE_ID --version 1.2.0 --percentage 20
  parcel-admin gray-delete --package game --id RULE_ID
  parcel-admin backup --db data/parcel.db --blobs data/blobs --out backups/2026-01-01
  parcel-admin restore --backup backups/2026-01-01 --db data/parcel.db --blobs data/blobs

Server and API key come from --server/--api-key or PARCEL_SERVER_URL/PARCEL_API_KEY.
`)
}

func runUpload(args []string) error {
	flags := flag.NewFlagSet("upload", flag.ContinueOnError)
	server := flags.String("server", defaultServer(), "server URL")
	apiKey := flags.String("api-key", os.Getenv("PARCEL_API_KEY"), "admin API key")
	packageName := flags.String("package", "", "package name")
	version := flags.String("version", "", "package version")
	manifest := flags.String("manifest", "", "manifest.bytes path")
	note := flags.String("note", "", "version note")
	var bundles stringList
	flags.Var(&bundles, "bundle", "bundle file path (repeatable)")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *packageName == "" || *version == "" || *manifest == "" {
		return errors.New("package, version and manifest are required")
	}
	bodyReader, bodyWriter := io.Pipe()
	writer := multipart.NewWriter(bodyWriter)
	writeErr := make(chan error, 1)
	go func() {
		err := writeUpload(writer, *version, *note, *manifest, bundles)
		if closeErr := writer.Close(); err == nil {
			err = closeErr
		}
		_ = bodyWriter.CloseWithError(err)
		writeErr <- err
	}()
	endpoint := strings.TrimRight(*server, "/") + "/api/v1/admin/packages/" +
		url.PathEscape(*packageName) + "/versions"
	request, err := http.NewRequest(http.MethodPost, endpoint, bodyReader)
	if err != nil {
		return err
	}
	request.Header.Set("Content-Type", writer.FormDataContentType())
	request.Header.Set("X-API-Key", *apiKey)
	response, err := client().Do(request)
	if err != nil {
		_ = bodyReader.CloseWithError(err)
		<-writeErr
		return err
	}
	defer response.Body.Close()
	if err := <-writeErr; err != nil {
		return err
	}
	return printResponse(response)
}

func writeUpload(writer *multipart.Writer, version, note, manifest string, bundles []string) error {
	if err := writer.WriteField("version", version); err != nil {
		return err
	}
	if note != "" {
		if err := writer.WriteField("note", note); err != nil {
			return err
		}
	}
	manifestFile, err := os.Open(manifest)
	if err != nil {
		return err
	}
	defer manifestFile.Close()
	manifestPart, err := writer.CreateFormFile("manifest", filepath.Base(manifest))
	if err != nil {
		return err
	}
	if _, err := io.Copy(manifestPart, manifestFile); err != nil {
		return err
	}
	for _, bundle := range bundles {
		file, err := os.Open(bundle)
		if err != nil {
			return err
		}
		part, err := writer.CreateFormFile("bundles", filepath.Base(bundle))
		if err == nil {
			_, err = io.Copy(part, file)
			_ = file.Close()
		} else {
			_ = file.Close()
		}
		if err != nil {
			return err
		}
	}
	return nil
}

func runPublish(args []string) error {
	flags := flag.NewFlagSet("publish", flag.ContinueOnError)
	server := flags.String("server", defaultServer(), "server URL")
	apiKey := flags.String("api-key", os.Getenv("PARCEL_API_KEY"), "admin API key")
	packageName := flags.String("package", "", "package name")
	version := flags.String("version", "", "package version")
	channel := flags.String("channel", "prod", "channel")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *packageName == "" || *version == "" {
		return errors.New("package and version are required")
	}
	endpoint := strings.TrimRight(*server, "/") + "/api/v1/admin/packages/" +
		url.PathEscape(*packageName) + "/versions/" + url.PathEscape(*version) + "/publish"
	return sendJSON(endpoint, *apiKey, map[string]any{"channel": *channel})
}

func runRollback(args []string) error {
	flags := flag.NewFlagSet("rollback", flag.ContinueOnError)
	server := flags.String("server", defaultServer(), "server URL")
	apiKey := flags.String("api-key", os.Getenv("PARCEL_API_KEY"), "admin API key")
	packageName := flags.String("package", "", "package name")
	version := flags.String("version", "", "target version (default: previous)")
	channel := flags.String("channel", "prod", "channel")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *packageName == "" {
		return errors.New("package is required")
	}
	endpoint := strings.TrimRight(*server, "/") + "/api/v1/admin/packages/" +
		url.PathEscape(*packageName) + "/rollback"
	return sendJSON(endpoint, *apiKey, map[string]any{"channel": *channel, "version": *version})
}

func runRenamePackage(args []string) error {
	flags := flag.NewFlagSet("rename-package", flag.ContinueOnError)
	server := flags.String("server", defaultServer(), "server URL")
	apiKey := flags.String("api-key", os.Getenv("PARCEL_API_KEY"), "admin API key")
	packageName := flags.String("package", "", "current package name")
	name := flags.String("name", "", "new package name")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *packageName == "" || *name == "" {
		return errors.New("package and name are required")
	}
	endpoint := strings.TrimRight(*server, "/") + "/api/v1/admin/packages/" +
		url.PathEscape(*packageName)
	return sendJSONMethod(endpoint, http.MethodPatch, *apiKey, map[string]any{"name": *name})
}

func runDeletePackage(args []string) error {
	flags := flag.NewFlagSet("delete-package", flag.ContinueOnError)
	server := flags.String("server", defaultServer(), "server URL")
	apiKey := flags.String("api-key", os.Getenv("PARCEL_API_KEY"), "admin API key")
	packageName := flags.String("package", "", "package name")
	confirm := flags.String("confirm", "", "must match package name")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *packageName == "" || *confirm == "" {
		return errors.New("package and confirm are required")
	}
	endpoint := strings.TrimRight(*server, "/") + "/api/v1/admin/packages/" +
		url.PathEscape(*packageName) + "?confirm=" + url.QueryEscape(*confirm)
	return sendJSONMethod(endpoint, http.MethodDelete, *apiKey, nil)
}

func runRenameVersion(args []string) error {
	flags := flag.NewFlagSet("rename-version", flag.ContinueOnError)
	server := flags.String("server", defaultServer(), "server URL")
	apiKey := flags.String("api-key", os.Getenv("PARCEL_API_KEY"), "admin API key")
	packageName := flags.String("package", "", "package name")
	version := flags.String("version", "", "current version")
	name := flags.String("name", "", "new version")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *packageName == "" || *version == "" || *name == "" {
		return errors.New("package, version and name are required")
	}
	endpoint := strings.TrimRight(*server, "/") + "/api/v1/admin/packages/" +
		url.PathEscape(*packageName) + "/versions/" + url.PathEscape(*version)
	return sendJSONMethod(endpoint, http.MethodPatch, *apiKey, map[string]any{"version": *name})
}

func runDeleteVersion(args []string) error {
	flags := flag.NewFlagSet("delete-version", flag.ContinueOnError)
	server := flags.String("server", defaultServer(), "server URL")
	apiKey := flags.String("api-key", os.Getenv("PARCEL_API_KEY"), "admin API key")
	packageName := flags.String("package", "", "package name")
	version := flags.String("version", "", "version")
	confirm := flags.String("confirm", "", "must match version")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *packageName == "" || *version == "" || *confirm == "" {
		return errors.New("package, version and confirm are required")
	}
	endpoint := strings.TrimRight(*server, "/") + "/api/v1/admin/packages/" +
		url.PathEscape(*packageName) + "/versions/" + url.PathEscape(*version) +
		"?confirm=" + url.QueryEscape(*confirm)
	return sendJSONMethod(endpoint, http.MethodDelete, *apiKey, nil)
}

func runGray(args []string) error {
	flags := flag.NewFlagSet("gray", flag.ContinueOnError)
	server := flags.String("server", defaultServer(), "server URL")
	apiKey := flags.String("api-key", os.Getenv("PARCEL_API_KEY"), "admin API key")
	packageName := flags.String("package", "", "package name")
	version := flags.String("version", "", "target version")
	percentage := flags.Int("percentage", 0, "percentage 0-100")
	whitelist := flags.String("whitelist", "", "comma-separated player ids")
	platforms := flags.String("platforms", "", "comma-separated platforms")
	minApp := flags.String("min-app-version", "", "minimum app version")
	priority := flags.Int("priority", 0, "priority")
	enabled := flags.Bool("enabled", true, "enable rule")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *packageName == "" || *version == "" {
		return errors.New("package and version are required")
	}
	body := grayPayload(*version, *percentage, *whitelist, *platforms, *minApp, *priority, *enabled)
	endpoint := strings.TrimRight(*server, "/") + "/api/v1/admin/packages/" +
		url.PathEscape(*packageName) + "/gray-rules"
	return sendJSON(endpoint, *apiKey, body)
}

func runGrayUpdate(args []string) error {
	flags := flag.NewFlagSet("gray-update", flag.ContinueOnError)
	server := flags.String("server", defaultServer(), "server URL")
	apiKey := flags.String("api-key", os.Getenv("PARCEL_API_KEY"), "admin API key")
	packageName := flags.String("package", "", "package name")
	id := flags.String("id", "", "gray rule id")
	version := flags.String("version", "", "target version")
	percentage := flags.Int("percentage", 0, "percentage 0-100")
	whitelist := flags.String("whitelist", "", "comma-separated player ids")
	platforms := flags.String("platforms", "", "comma-separated platforms")
	minApp := flags.String("min-app-version", "", "minimum app version")
	priority := flags.Int("priority", 0, "priority")
	enabled := flags.Bool("enabled", true, "enable rule")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *packageName == "" || *id == "" || *version == "" {
		return errors.New("package, id and version are required")
	}
	body := grayPayload(*version, *percentage, *whitelist, *platforms, *minApp, *priority, *enabled)
	endpoint := strings.TrimRight(*server, "/") + "/api/v1/admin/packages/" +
		url.PathEscape(*packageName) + "/gray-rules/" + url.PathEscape(*id)
	return sendJSONMethod(endpoint, http.MethodPut, *apiKey, body)
}

func runGrayDelete(args []string) error {
	flags := flag.NewFlagSet("gray-delete", flag.ContinueOnError)
	server := flags.String("server", defaultServer(), "server URL")
	apiKey := flags.String("api-key", os.Getenv("PARCEL_API_KEY"), "admin API key")
	packageName := flags.String("package", "", "package name")
	id := flags.String("id", "", "gray rule id")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *packageName == "" || *id == "" {
		return errors.New("package and id are required")
	}
	endpoint := strings.TrimRight(*server, "/") + "/api/v1/admin/packages/" +
		url.PathEscape(*packageName) + "/gray-rules/" + url.PathEscape(*id)
	return sendJSONMethod(endpoint, http.MethodDelete, *apiKey, nil)
}

func grayPayload(version string, percentage int, whitelist, platforms, minApp string,
	priority int, enabled bool) map[string]any {
	return map[string]any{
		"version":         version,
		"percentage":      percentage,
		"whitelist":       splitCSV(whitelist),
		"platforms":       splitCSV(platforms),
		"min_app_version": minApp,
		"priority":        priority,
		"enabled":         enabled,
	}
}

func runBackup(args []string) error {
	flags := flag.NewFlagSet("backup", flag.ContinueOnError)
	db := flags.String("db", "data/parcel.db", "SQLite database path")
	blobs := flags.String("blobs", "data/blobs", "blob directory")
	out := flags.String("out", "", "output directory")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *out == "" {
		*out = "backups/" + time.Now().UTC().Format("20060102T150405Z")
	}
	manifest, err := backup.Create(*db, *blobs, *out)
	if err != nil {
		return err
	}
	return printJSON(manifest)
}

func runRestore(args []string) error {
	flags := flag.NewFlagSet("restore", flag.ContinueOnError)
	backupDir := flags.String("backup", "", "backup directory")
	db := flags.String("db", "data/parcel.db", "SQLite database path")
	blobs := flags.String("blobs", "data/blobs", "blob directory")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *backupDir == "" {
		return errors.New("backup is required")
	}
	if err := backup.Restore(*backupDir, *db, *blobs); err != nil {
		return err
	}
	fmt.Println("restore complete; restart the server")
	return nil
}

func sendJSON(endpoint, apiKey string, body any) error {
	return sendJSONMethod(endpoint, http.MethodPost, apiKey, body)
}

func sendJSONMethod(endpoint, method, apiKey string, body any) error {
	var reader io.Reader
	if body != nil {
		payload, err := json.Marshal(body)
		if err != nil {
			return err
		}
		reader = bytes.NewReader(payload)
	}
	request, err := http.NewRequest(method, endpoint, reader)
	if err != nil {
		return err
	}
	if body != nil {
		request.Header.Set("Content-Type", "application/json")
	}
	request.Header.Set("X-API-Key", apiKey)
	response, err := client().Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	return printResponse(response)
}

func printResponse(response *http.Response) error {
	value, err := io.ReadAll(io.LimitReader(response.Body, 4<<20))
	if err != nil {
		return err
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return fmt.Errorf("server returned %s: %s", response.Status, strings.TrimSpace(string(value)))
	}
	var pretty bytes.Buffer
	if json.Indent(&pretty, value, "", "  ") == nil {
		fmt.Println(pretty.String())
	} else {
		fmt.Print(string(value))
	}
	return nil
}

func printJSON(value any) error {
	encoded, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	fmt.Println(string(encoded))
	return nil
}

func defaultServer() string {
	if value := strings.TrimSpace(os.Getenv("PARCEL_SERVER_URL")); value != "" {
		return value
	}
	return "http://localhost:8080"
}

func client() *http.Client {
	return &http.Client{Timeout: 10 * time.Minute}
}

func splitCSV(value string) []string {
	var result []string
	for _, item := range strings.Split(value, ",") {
		if item = strings.TrimSpace(item); item != "" {
			result = append(result, item)
		}
	}
	return result
}
