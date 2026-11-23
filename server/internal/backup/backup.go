package backup

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	_ "modernc.org/sqlite"
)

type Manifest struct {
	CreatedAt string `json:"created_at"`
	Database  string `json:"database"`
	Blobs     string `json:"blobs"`
}

func Create(dbPath, blobRoot, outputDir string) (Manifest, error) {
	if dbPath == "" || blobRoot == "" || outputDir == "" {
		return Manifest{}, fmt.Errorf("db, blobs and out are required")
	}
	if err := os.MkdirAll(outputDir, 0o755); err != nil {
		return Manifest{}, err
	}
	dbCopy := filepath.Join(outputDir, "parcel.db")
	_ = os.Remove(dbCopy)
	db, err := sql.Open("sqlite", dbPath)
	if err != nil {
		return Manifest{}, err
	}
	escaped := strings.ReplaceAll(dbCopy, "'", "''")
	_, err = db.Exec("VACUUM INTO '" + escaped + "'")
	closeErr := db.Close()
	if err != nil {
		return Manifest{}, fmt.Errorf("snapshot database: %w", err)
	}
	if closeErr != nil {
		return Manifest{}, closeErr
	}
	blobCopy := filepath.Join(outputDir, "blobs")
	if err := copyTree(blobRoot, blobCopy); err != nil {
		return Manifest{}, err
	}
	manifest := Manifest{
		CreatedAt: time.Now().UTC().Format(time.RFC3339),
		Database:  "parcel.db",
		Blobs:     "blobs",
	}
	value, _ := json.MarshalIndent(manifest, "", "  ")
	if err := os.WriteFile(filepath.Join(outputDir, "backup.json"), append(value, '\n'), 0o644); err != nil {
		return Manifest{}, err
	}
	return manifest, nil
}

func copyTree(source, destination string) error {
	sourceInfo, err := os.Stat(source)
	if err != nil {
		return err
	}
	if !sourceInfo.IsDir() {
		return fmt.Errorf("blob root is not a directory")
	}
	return filepath.WalkDir(source, func(current string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		relative, err := filepath.Rel(source, current)
		if err != nil {
			return err
		}
		if relative == "." {
			return os.MkdirAll(destination, 0o755)
		}
		if entry.IsDir() {
			if entry.Name() == ".tmp" {
				return filepath.SkipDir
			}
			return os.MkdirAll(filepath.Join(destination, relative), 0o755)
		}
		if !entry.Type().IsRegular() {
			return nil
		}
		input, err := os.Open(current)
		if err != nil {
			return err
		}
		target := filepath.Join(destination, relative)
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		output, err := os.OpenFile(target, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
		if err != nil {
			return err
		}
		_, copyErr := io.Copy(output, input)
		inputErr := input.Close()
		closeErr := output.Close()
		if inputErr != nil {
			return inputErr
		}
		if copyErr != nil {
			return copyErr
		}
		return closeErr
	})
}
func Restore(backupDir, dbPath, blobRoot string) error {
	if backupDir == "" || dbPath == "" || blobRoot == "" {
		return fmt.Errorf("backup, db and blobs are required")
	}
	manifestBytes, err := os.ReadFile(filepath.Join(backupDir, "backup.json"))
	if err != nil {
		return fmt.Errorf("read backup manifest: %w", err)
	}
	var manifest Manifest
	if err := json.Unmarshal(manifestBytes, &manifest); err != nil {
		return fmt.Errorf("parse backup manifest: %w", err)
	}
	sourceDB := filepath.Join(backupDir, manifest.Database)
	if _, err := os.Stat(sourceDB); err != nil {
		return fmt.Errorf("backup database: %w", err)
	}
	if err := os.MkdirAll(filepath.Dir(dbPath), 0o755); err != nil {
		return err
	}
	tempDB := dbPath + ".restore"
	if err := copyFile(sourceDB, tempDB); err != nil {
		return err
	}
	if err := os.Rename(tempDB, dbPath); err != nil {
		_ = os.Remove(tempDB)
		return fmt.Errorf("replace database: %w", err)
	}
	if err := copyTree(filepath.Join(backupDir, manifest.Blobs), blobRoot); err != nil {
		return fmt.Errorf("restore blobs: %w", err)
	}
	return nil
}

func copyFile(source, destination string) error {
	input, err := os.Open(source)
	if err != nil {
		return err
	}
	defer input.Close()
	output, err := os.OpenFile(destination, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
	if err != nil {
		return err
	}
	_, copyErr := io.Copy(output, input)
	closeErr := output.Close()
	if copyErr != nil {
		return copyErr
	}
	return closeErr
}
