package store

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"time"
)

func (s *Store) CreatePackage(ctx context.Context, name, createdBy string) (Package, error) {
	name = normalize(name)
	if name == "" {
		return Package{}, ErrInvalid
	}
	value := Package{ID: NewID(), Name: name, CreatedAt: time.Now().UTC()}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Package{}, err
	}
	if _, err := tx.ExecContext(ctx,
		"INSERT INTO packages(id, name, created_at, created_by) VALUES(?, ?, ?, ?)",
		value.ID, value.Name, timeText(value.CreatedAt), createdBy); err != nil {
		_ = tx.Rollback()
		if isUniqueError(err) {
			return Package{}, ErrConflict
		}
		return Package{}, err
	}
	if _, err := tx.ExecContext(ctx,
		"INSERT INTO channels(id, package_id, name, updated_at) VALUES(?, ?, 'prod', ?)",
		NewID(), value.ID, nowText()); err != nil {
		_ = tx.Rollback()
		return Package{}, err
	}
	if err := tx.Commit(); err != nil {
		return Package{}, err
	}
	return value, nil
}

func (s *Store) ListPackages(ctx context.Context) ([]Package, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT p.id, p.name, p.created_at,
                (SELECT COUNT(1) FROM versions v WHERE v.package_id = p.id)
         FROM packages p ORDER BY p.name`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var values []Package
	for rows.Next() {
		var value Package
		var createdAt string
		if err := rows.Scan(&value.ID, &value.Name, &createdAt, &value.VersionCount); err != nil {
			return nil, err
		}
		value.CreatedAt = parseTime(createdAt)
		values = append(values, value)
	}
	return values, rows.Err()
}

func (s *Store) PackageByName(ctx context.Context, name string) (Package, error) {
	var value Package
	var createdAt string
	err := s.db.QueryRowContext(ctx,
		"SELECT id, name, created_at FROM packages WHERE name = ?", name).
		Scan(&value.ID, &value.Name, &createdAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Package{}, ErrNotFound
	}
	if err != nil {
		return Package{}, err
	}
	value.CreatedAt = parseTime(createdAt)
	return value, nil
}

func (s *Store) CreateVersion(ctx context.Context, packageName, version, manifestHash, note string,
	artifacts []ArtifactInput) (Version, error) {
	pkg, err := s.PackageByName(ctx, packageName)
	if err != nil {
		return Version{}, err
	}
	version = normalize(version)
	if version == "" {
		return Version{}, ErrInvalid
	}
	value := Version{
		ID:           NewID(),
		PackageID:    pkg.ID,
		Package:      pkg.Name,
		Version:      version,
		State:        "draft",
		ManifestHash: manifestHash,
		Note:         note,
		CreatedAt:    time.Now().UTC(),
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Version{}, err
	}
	if _, err := tx.ExecContext(ctx,
		`INSERT INTO versions(id, package_id, version, state, manifest_hash, note, created_at)
         VALUES(?, ?, ?, 'draft', ?, ?, ?)`,
		value.ID, value.PackageID, value.Version, value.ManifestHash, value.Note,
		timeText(value.CreatedAt)); err != nil {
		_ = tx.Rollback()
		if isUniqueError(err) {
			return Version{}, ErrConflict
		}
		return Version{}, err
	}
	seen := make(map[string]struct{}, len(artifacts))
	for _, input := range artifacts {
		if input.Name == "" || input.SHA256 == "" || input.Size < 0 {
			_ = tx.Rollback()
			return Version{}, ErrInvalid
		}
		if _, exists := seen[input.Name]; exists {
			_ = tx.Rollback()
			return Version{}, fmt.Errorf("%w: duplicate artifact %s", ErrInvalid, input.Name)
		}
		seen[input.Name] = struct{}{}
		artifact := Artifact{
			ID:        NewID(),
			Kind:      input.Kind,
			Name:      input.Name,
			SHA256:    input.SHA256,
			Size:      input.Size,
			CreatedAt: value.CreatedAt,
		}
		if _, err := tx.ExecContext(ctx,
			`INSERT INTO artifacts(id, version_id, kind, name, sha256, size, created_at)
             VALUES(?, ?, ?, ?, ?, ?, ?)`,
			artifact.ID, value.ID, artifact.Kind, artifact.Name, artifact.SHA256,
			artifact.Size, timeText(artifact.CreatedAt)); err != nil {
			_ = tx.Rollback()
			if isUniqueError(err) {
				return Version{}, ErrConflict
			}
			return Version{}, err
		}
		value.Artifacts = append(value.Artifacts, artifact)
	}
	if err := tx.Commit(); err != nil {
		return Version{}, err
	}
	return value, nil
}

func (s *Store) GetVersion(ctx context.Context, packageName, version string) (Version, error) {
	row := s.db.QueryRowContext(ctx,
		`SELECT v.id, v.package_id, p.name, v.version, v.state, v.manifest_hash, v.note,
                v.created_at, v.published_at
         FROM versions v JOIN packages p ON p.id = v.package_id
         WHERE p.name = ? AND v.version = ?`, packageName, version)
	value, err := scanVersion(row)
	if err != nil {
		return Version{}, err
	}
	value.Artifacts, err = s.ListArtifacts(ctx, value.ID)
	return value, err
}

func (s *Store) VersionByID(ctx context.Context, id string) (Version, error) {
	row := s.db.QueryRowContext(ctx,
		`SELECT v.id, v.package_id, p.name, v.version, v.state, v.manifest_hash, v.note,
                v.created_at, v.published_at
         FROM versions v JOIN packages p ON p.id = v.package_id
         WHERE v.id = ?`, id)
	return scanVersion(row)
}

func (s *Store) ListVersions(ctx context.Context, packageName string) ([]Version, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT v.id, v.package_id, p.name, v.version, v.state, v.manifest_hash, v.note,
                v.created_at, v.published_at
         FROM versions v JOIN packages p ON p.id = v.package_id
         WHERE p.name = ? ORDER BY v.created_at DESC, v.version DESC`, packageName)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var values []Version
	for rows.Next() {
		value, err := scanVersion(rows)
		if err != nil {
			return nil, err
		}
		values = append(values, value)
	}
	return values, rows.Err()
}

func (s *Store) ListArtifacts(ctx context.Context, versionID string) ([]Artifact, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT id, kind, name, sha256, size, created_at FROM artifacts
         WHERE version_id = ? ORDER BY name`, versionID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var values []Artifact
	for rows.Next() {
		var value Artifact
		var createdAt string
		if err := rows.Scan(&value.ID, &value.Kind, &value.Name, &value.SHA256, &value.Size, &createdAt); err != nil {
			return nil, err
		}
		value.CreatedAt = parseTime(createdAt)
		values = append(values, value)
	}
	return values, rows.Err()
}

func (s *Store) ArtifactByName(ctx context.Context, packageName, version, name string) (Artifact, error) {
	var value Artifact
	var createdAt string
	err := s.db.QueryRowContext(ctx,
		`SELECT a.id, a.kind, a.name, a.sha256, a.size, a.created_at
         FROM artifacts a
         JOIN versions v ON v.id = a.version_id
         JOIN packages p ON p.id = v.package_id
         WHERE p.name = ? AND v.version = ? AND a.name = ?`,
		packageName, version, name).
		Scan(&value.ID, &value.Kind, &value.Name, &value.SHA256, &value.Size, &createdAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Artifact{}, ErrNotFound
	}
	if err != nil {
		return Artifact{}, err
	}
	value.CreatedAt = parseTime(createdAt)
	return value, nil
}

func (s *Store) Publish(ctx context.Context, packageName, version, channel string) (Channel, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Channel{}, err
	}
	pkg, err := packageTx(ctx, tx, packageName)
	if err != nil {
		_ = tx.Rollback()
		return Channel{}, err
	}
	versionID, err := versionIDTx(ctx, tx, pkg.ID, version)
	if err != nil {
		_ = tx.Rollback()
		return Channel{}, err
	}
	var manifestCount int
	if err := tx.QueryRowContext(ctx,
		"SELECT COUNT(1) FROM artifacts WHERE version_id = ? AND kind = 'manifest'", versionID).
		Scan(&manifestCount); err != nil {
		_ = tx.Rollback()
		return Channel{}, err
	}
	if manifestCount == 0 {
		_ = tx.Rollback()
		return Channel{}, ErrInvalid
	}
	if channel == "" {
		channel = "prod"
	}
	now := time.Now().UTC()
	if _, err := tx.ExecContext(ctx,
		`INSERT INTO channels(id, package_id, name, version_id, updated_at)
         VALUES(?, ?, ?, ?, ?)
         ON CONFLICT(package_id, name) DO UPDATE SET version_id = excluded.version_id,
             updated_at = excluded.updated_at`,
		NewID(), pkg.ID, channel, versionID, timeText(now)); err != nil {
		_ = tx.Rollback()
		return Channel{}, err
	}
	if _, err := tx.ExecContext(ctx,
		`UPDATE versions SET state = 'published',
             published_at = COALESCE(published_at, ?)
         WHERE id = ?`, timeText(now), versionID); err != nil {
		_ = tx.Rollback()
		return Channel{}, err
	}
	if err := tx.Commit(); err != nil {
		return Channel{}, err
	}
	return s.ChannelByName(ctx, packageName, channel)
}

func (s *Store) Rollback(ctx context.Context, packageName, channel, targetVersion string) (Channel, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Channel{}, err
	}
	pkg, err := packageTx(ctx, tx, packageName)
	if err != nil {
		_ = tx.Rollback()
		return Channel{}, err
	}
	if channel == "" {
		channel = "prod"
	}
	var currentVersionID sql.NullString
	var currentVersion string
	err = tx.QueryRowContext(ctx,
		`SELECT c.version_id, COALESCE(v.version, '') FROM channels c
         LEFT JOIN versions v ON v.id = c.version_id
         WHERE c.package_id = ? AND c.name = ?`, pkg.ID, channel).
		Scan(&currentVersionID, &currentVersion)
	if errors.Is(err, sql.ErrNoRows) {
		currentVersionID = sql.NullString{}
		err = nil
	}
	if err != nil {
		_ = tx.Rollback()
		return Channel{}, err
	}
	var targetID, target string
	if targetVersion == "" {
		err = tx.QueryRowContext(ctx,
			`SELECT id, version FROM versions
             WHERE package_id = ? AND published_at IS NOT NULL AND id <> ?
             ORDER BY published_at DESC, created_at DESC LIMIT 1`,
			pkg.ID, currentVersionID.String).Scan(&targetID, &target)
		if errors.Is(err, sql.ErrNoRows) {
			_ = tx.Rollback()
			return Channel{}, ErrNotFound
		}
	} else {
		err = tx.QueryRowContext(ctx,
			"SELECT id, version FROM versions WHERE package_id = ? AND version = ?",
			pkg.ID, targetVersion).Scan(&targetID, &target)
		if errors.Is(err, sql.ErrNoRows) {
			_ = tx.Rollback()
			return Channel{}, ErrNotFound
		}
	}
	if err != nil {
		_ = tx.Rollback()
		return Channel{}, err
	}
	now := time.Now().UTC()
	if currentVersionID.Valid && currentVersionID.String != targetID {
		if _, err := tx.ExecContext(ctx,
			"UPDATE versions SET state = 'rolled_back' WHERE id = ? AND state = 'published'",
			currentVersionID.String); err != nil {
			_ = tx.Rollback()
			return Channel{}, err
		}
	}
	if _, err := tx.ExecContext(ctx,
		`UPDATE versions SET state = 'published', published_at = COALESCE(published_at, ?)
         WHERE id = ?`, timeText(now), targetID); err != nil {
		_ = tx.Rollback()
		return Channel{}, err
	}
	if _, err := tx.ExecContext(ctx,
		`INSERT INTO channels(id, package_id, name, version_id, updated_at)
         VALUES(?, ?, ?, ?, ?)
         ON CONFLICT(package_id, name) DO UPDATE SET version_id = excluded.version_id,
             updated_at = excluded.updated_at`,
		NewID(), pkg.ID, channel, targetID, timeText(now)); err != nil {
		_ = tx.Rollback()
		return Channel{}, err
	}
	if err := tx.Commit(); err != nil {
		return Channel{}, err
	}
	_ = currentVersion
	return s.ChannelByName(ctx, packageName, channel)
}

func (s *Store) SetChannel(ctx context.Context, packageName, channel, version string) (Channel, error) {
	pkg, err := s.PackageByName(ctx, packageName)
	if err != nil {
		return Channel{}, err
	}
	versionID, err := s.versionID(ctx, pkg.ID, version)
	if err != nil {
		return Channel{}, err
	}
	if channel == "" {
		channel = "prod"
	}
	_, err = s.db.ExecContext(ctx,
		`INSERT INTO channels(id, package_id, name, version_id, updated_at)
         VALUES(?, ?, ?, ?, ?)
         ON CONFLICT(package_id, name) DO UPDATE SET version_id = excluded.version_id,
             updated_at = excluded.updated_at`,
		NewID(), pkg.ID, channel, versionID, nowText())
	if err != nil {
		return Channel{}, err
	}
	return s.ChannelByName(ctx, packageName, channel)
}

func (s *Store) ListChannels(ctx context.Context, packageName string) ([]Channel, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT c.id, c.package_id, c.name, c.version_id, COALESCE(v.version, ''), c.updated_at
         FROM channels c
         JOIN packages p ON p.id = c.package_id
         LEFT JOIN versions v ON v.id = c.version_id
         WHERE p.name = ? ORDER BY c.name`, packageName)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var values []Channel
	for rows.Next() {
		var value Channel
		var versionID sql.NullString
		var updatedAt string
		if err := rows.Scan(&value.ID, &value.PackageID, &value.Name, &versionID, &value.Version, &updatedAt); err != nil {
			return nil, err
		}
		if versionID.Valid {
			id := versionID.String
			value.VersionID = &id
		}
		value.UpdatedAt = parseTime(updatedAt)
		values = append(values, value)
	}
	return values, rows.Err()
}

func (s *Store) ChannelByName(ctx context.Context, packageName, channel string) (Channel, error) {
	var value Channel
	var versionID sql.NullString
	var updatedAt string
	err := s.db.QueryRowContext(ctx,
		`SELECT c.id, c.package_id, c.name, c.version_id, COALESCE(v.version, ''), c.updated_at
         FROM channels c
         JOIN packages p ON p.id = c.package_id
         LEFT JOIN versions v ON v.id = c.version_id
         WHERE p.name = ? AND c.name = ?`, packageName, channel).
		Scan(&value.ID, &value.PackageID, &value.Name, &versionID, &value.Version, &updatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Channel{}, ErrNotFound
	}
	if err != nil {
		return Channel{}, err
	}
	if versionID.Valid {
		id := versionID.String
		value.VersionID = &id
	}
	value.UpdatedAt = parseTime(updatedAt)
	return value, nil
}

func (s *Store) PreviousVersion(ctx context.Context, packageName, currentVersionID string) (string, error) {
	var version string
	err := s.db.QueryRowContext(ctx,
		`SELECT v.version FROM versions v JOIN packages p ON p.id = v.package_id
         WHERE p.name = ? AND v.published_at IS NOT NULL AND v.id <> ?
         ORDER BY v.published_at DESC, v.created_at DESC LIMIT 1`,
		packageName, currentVersionID).Scan(&version)
	if errors.Is(err, sql.ErrNoRows) {
		return "", nil
	}
	return version, err
}

func (s *Store) versionID(ctx context.Context, packageID, version string) (string, error) {
	return versionIDQuery(ctx, s.db, packageID, version)
}

type queryer interface {
	QueryRowContext(context.Context, string, ...any) *sql.Row
}

func packageTx(ctx context.Context, tx *sql.Tx, name string) (Package, error) {
	var value Package
	var createdAt string
	err := tx.QueryRowContext(ctx, "SELECT id, name, created_at FROM packages WHERE name = ?", name).
		Scan(&value.ID, &value.Name, &createdAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Package{}, ErrNotFound
	}
	if err != nil {
		return Package{}, err
	}
	value.CreatedAt = parseTime(createdAt)
	return value, nil
}

func versionIDTx(ctx context.Context, tx *sql.Tx, packageID, version string) (string, error) {
	return versionIDQuery(ctx, tx, packageID, version)
}

func versionIDQuery(ctx context.Context, q queryer, packageID, version string) (string, error) {
	var id string
	err := q.QueryRowContext(ctx,
		"SELECT id FROM versions WHERE package_id = ? AND version = ?", packageID, version).Scan(&id)
	if errors.Is(err, sql.ErrNoRows) {
		return "", ErrNotFound
	}
	return id, err
}

type rowScanner interface {
	Scan(...any) error
}

func scanVersion(row rowScanner) (Version, error) {
	var value Version
	var createdAt string
	var publishedAt sql.NullString
	err := row.Scan(&value.ID, &value.PackageID, &value.Package, &value.Version, &value.State,
		&value.ManifestHash, &value.Note, &createdAt, &publishedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Version{}, ErrNotFound
	}
	if err != nil {
		return Version{}, err
	}
	value.CreatedAt = parseTime(createdAt)
	value.PublishedAt = parseNullTime(publishedAt)
	return value, nil
}
