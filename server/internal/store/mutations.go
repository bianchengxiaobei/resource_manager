package store

import (
	"context"
	"database/sql"
	"strings"
)

func (s *Store) RenamePackage(ctx context.Context, oldName, newName string) (Package, error) {
	oldName = normalize(oldName)
	newName = normalize(newName)
	if oldName == "" || newName == "" {
		return Package{}, ErrInvalid
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Package{}, err
	}
	pkg, err := packageTx(ctx, tx, oldName)
	if err != nil {
		_ = tx.Rollback()
		return Package{}, err
	}
	if oldName == newName {
		_ = tx.Rollback()
		return s.PackageByName(ctx, oldName)
	}
	if _, err := tx.ExecContext(ctx, "UPDATE packages SET name = ? WHERE id = ?", newName, pkg.ID); err != nil {
		_ = tx.Rollback()
		if isUniqueError(err) {
			return Package{}, ErrConflict
		}
		return Package{}, err
	}
	versions, err := packageVersionsTx(ctx, tx, pkg.ID)
	if err != nil {
		_ = tx.Rollback()
		return Package{}, err
	}
	for _, item := range versions {
		oldPrefix := oldName + "_" + item.version
		newPrefix := newName + "_" + item.version
		if err := renameArtifactsTx(ctx, tx, item.id, oldPrefix, newPrefix); err != nil {
			_ = tx.Rollback()
			return Package{}, err
		}
	}
	var count int
	if err := tx.QueryRowContext(ctx,
		"SELECT COUNT(1) FROM versions WHERE package_id = ?", pkg.ID).Scan(&count); err != nil {
		_ = tx.Rollback()
		return Package{}, err
	}
	if err := tx.Commit(); err != nil {
		return Package{}, err
	}
	return Package{ID: pkg.ID, Name: newName, CreatedAt: pkg.CreatedAt, VersionCount: count}, nil
}

func (s *Store) DeletePackage(ctx context.Context, name string) ([]string, error) {
	name = normalize(name)
	if name == "" {
		return nil, ErrInvalid
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	pkg, err := packageTx(ctx, tx, name)
	if err != nil {
		_ = tx.Rollback()
		return nil, err
	}
	shas, err := packageArtifactSHAsTx(ctx, tx, pkg.ID)
	if err != nil {
		_ = tx.Rollback()
		return nil, err
	}
	for _, statement := range []string{
		"DELETE FROM gray_rules WHERE package_id = ?",
		"DELETE FROM channels WHERE package_id = ?",
		"DELETE FROM artifacts WHERE version_id IN (SELECT id FROM versions WHERE package_id = ?)",
		"DELETE FROM versions WHERE package_id = ?",
		"DELETE FROM packages WHERE id = ?",
	} {
		if _, err := tx.ExecContext(ctx, statement, pkg.ID); err != nil {
			_ = tx.Rollback()
			return nil, err
		}
	}
	if err := tx.Commit(); err != nil {
		return nil, err
	}
	return shas, nil
}

func (s *Store) RenameVersion(ctx context.Context, packageName, oldVersion, newVersion string) (Version, error) {
	packageName = normalize(packageName)
	oldVersion = normalize(oldVersion)
	newVersion = normalize(newVersion)
	if packageName == "" || oldVersion == "" || newVersion == "" {
		return Version{}, ErrInvalid
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Version{}, err
	}
	pkg, err := packageTx(ctx, tx, packageName)
	if err != nil {
		_ = tx.Rollback()
		return Version{}, err
	}
	versionID, err := versionIDTx(ctx, tx, pkg.ID, oldVersion)
	if err != nil {
		_ = tx.Rollback()
		return Version{}, err
	}
	if oldVersion == newVersion {
		_ = tx.Rollback()
		return s.GetVersion(ctx, packageName, oldVersion)
	}
	if _, err := tx.ExecContext(ctx,
		"UPDATE versions SET version = ? WHERE id = ?", newVersion, versionID); err != nil {
		_ = tx.Rollback()
		if isUniqueError(err) {
			return Version{}, ErrConflict
		}
		return Version{}, err
	}
	if err := renameArtifactsTx(ctx, tx, versionID,
		pkg.Name+"_"+oldVersion, pkg.Name+"_"+newVersion); err != nil {
		_ = tx.Rollback()
		return Version{}, err
	}
	if err := tx.Commit(); err != nil {
		return Version{}, err
	}
	return s.GetVersion(ctx, packageName, newVersion)
}

func (s *Store) DeleteVersion(ctx context.Context, packageName, version string) ([]string, error) {
	packageName = normalize(packageName)
	version = normalize(version)
	if packageName == "" || version == "" {
		return nil, ErrInvalid
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	pkg, err := packageTx(ctx, tx, packageName)
	if err != nil {
		_ = tx.Rollback()
		return nil, err
	}
	versionID, err := versionIDTx(ctx, tx, pkg.ID, version)
	if err != nil {
		_ = tx.Rollback()
		return nil, err
	}
	var references int
	if err := tx.QueryRowContext(ctx,
		`SELECT (SELECT COUNT(1) FROM channels WHERE version_id = ?) +
                (SELECT COUNT(1) FROM gray_rules WHERE version_id = ?)`,
		versionID, versionID).Scan(&references); err != nil {
		_ = tx.Rollback()
		return nil, err
	}
	if references > 0 {
		_ = tx.Rollback()
		return nil, ErrInUse
	}
	shas, err := versionArtifactSHAsTx(ctx, tx, versionID)
	if err != nil {
		_ = tx.Rollback()
		return nil, err
	}
	if _, err := tx.ExecContext(ctx, "DELETE FROM artifacts WHERE version_id = ?", versionID); err != nil {
		_ = tx.Rollback()
		return nil, err
	}
	if _, err := tx.ExecContext(ctx, "DELETE FROM versions WHERE id = ?", versionID); err != nil {
		_ = tx.Rollback()
		return nil, err
	}
	if err := tx.Commit(); err != nil {
		return nil, err
	}
	return shas, nil
}

func (s *Store) ArtifactSHAExists(ctx context.Context, sha string) (bool, error) {
	var exists int
	if err := s.db.QueryRowContext(ctx,
		"SELECT EXISTS(SELECT 1 FROM artifacts WHERE sha256 = ?)", sha).Scan(&exists); err != nil {
		return false, err
	}
	return exists != 0, nil
}

type packageVersion struct {
	id      string
	version string
}

func packageVersionsTx(ctx context.Context, tx *sql.Tx, packageID string) ([]packageVersion, error) {
	rows, err := tx.QueryContext(ctx,
		"SELECT id, version FROM versions WHERE package_id = ?", packageID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var values []packageVersion
	for rows.Next() {
		var value packageVersion
		if err := rows.Scan(&value.id, &value.version); err != nil {
			return nil, err
		}
		values = append(values, value)
	}
	return values, rows.Err()
}

func renameArtifactsTx(ctx context.Context, tx *sql.Tx, versionID, oldPrefix, newPrefix string) error {
	rows, err := tx.QueryContext(ctx,
		`SELECT id, name FROM artifacts
         WHERE version_id = ? AND kind IN ('manifest', 'manifest_hash')`, versionID)
	if err != nil {
		return err
	}
	type update struct {
		id   string
		name string
	}
	var updates []update
	for rows.Next() {
		var id, name string
		if err := rows.Scan(&id, &name); err != nil {
			rows.Close()
			return err
		}
		if !strings.HasPrefix(name, oldPrefix) {
			continue
		}
		updates = append(updates, update{id: id, name: newPrefix + strings.TrimPrefix(name, oldPrefix)})
	}
	if err := rows.Close(); err != nil {
		return err
	}
	if err := rows.Err(); err != nil {
		return err
	}
	for _, item := range updates {
		if _, err := tx.ExecContext(ctx, "UPDATE artifacts SET name = ? WHERE id = ?", item.name, item.id); err != nil {
			if isUniqueError(err) {
				return ErrConflict
			}
			return err
		}
	}
	return nil
}

func packageArtifactSHAsTx(ctx context.Context, tx *sql.Tx, packageID string) ([]string, error) {
	rows, err := tx.QueryContext(ctx,
		`SELECT DISTINCT a.sha256 FROM artifacts a
         JOIN versions v ON v.id = a.version_id
         WHERE v.package_id = ?`, packageID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return scanSHAs(rows)
}

func versionArtifactSHAsTx(ctx context.Context, tx *sql.Tx, versionID string) ([]string, error) {
	rows, err := tx.QueryContext(ctx,
		"SELECT DISTINCT sha256 FROM artifacts WHERE version_id = ?", versionID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return scanSHAs(rows)
}

type shaScanner interface {
	Next() bool
	Scan(...any) error
	Err() error
}

func scanSHAs(rows shaScanner) ([]string, error) {
	var values []string
	for rows.Next() {
		var value string
		if err := rows.Scan(&value); err != nil {
			return nil, err
		}
		values = append(values, value)
	}
	return values, rows.Err()
}
