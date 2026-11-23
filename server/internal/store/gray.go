package store

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"time"
)

type GrayRuleInput struct {
	Version       string
	Percentage    int
	Whitelist     []string
	Platforms     []string
	MinAppVersion string
	Priority      int
	Enabled       bool
}

func (s *Store) CreateGrayRule(ctx context.Context, packageName string, input GrayRuleInput) (GrayRule, error) {
	pkg, err := s.PackageByName(ctx, packageName)
	if err != nil {
		return GrayRule{}, err
	}
	versionID, err := s.versionID(ctx, pkg.ID, input.Version)
	if err != nil {
		return GrayRule{}, err
	}
	if input.Percentage < 0 || input.Percentage > 100 {
		return GrayRule{}, ErrInvalid
	}
	whitelist, err := json.Marshal(cleanStrings(input.Whitelist))
	if err != nil {
		return GrayRule{}, err
	}
	platforms, err := json.Marshal(cleanStrings(input.Platforms))
	if err != nil {
		return GrayRule{}, err
	}
	now := time.Now().UTC()
	value := GrayRule{
		ID:            NewID(),
		PackageID:     pkg.ID,
		VersionID:     versionID,
		Version:       input.Version,
		Percentage:    input.Percentage,
		Whitelist:     cleanStrings(input.Whitelist),
		Platforms:     cleanStrings(input.Platforms),
		MinAppVersion: input.MinAppVersion,
		Priority:      input.Priority,
		Enabled:       input.Enabled,
		CreatedAt:     now,
		UpdatedAt:     now,
	}
	_, err = s.db.ExecContext(ctx,
		`INSERT INTO gray_rules(id, package_id, version_id, percentage, whitelist, platforms,
             min_app_version, priority, enabled, created_at, updated_at)
         VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		value.ID, value.PackageID, value.VersionID, value.Percentage, string(whitelist),
		string(platforms), value.MinAppVersion, value.Priority, boolInt(value.Enabled),
		timeText(now), timeText(now))
	if err != nil {
		return GrayRule{}, err
	}
	return value, nil
}

func (s *Store) UpdateGrayRule(ctx context.Context, packageName, id string, input GrayRuleInput) (GrayRule, error) {
	pkg, err := s.PackageByName(ctx, packageName)
	if err != nil {
		return GrayRule{}, err
	}
	versionID, err := s.versionID(ctx, pkg.ID, input.Version)
	if err != nil {
		return GrayRule{}, err
	}
	if input.Percentage < 0 || input.Percentage > 100 {
		return GrayRule{}, ErrInvalid
	}
	whitelist, _ := json.Marshal(cleanStrings(input.Whitelist))
	platforms, _ := json.Marshal(cleanStrings(input.Platforms))
	now := time.Now().UTC()
	result, err := s.db.ExecContext(ctx,
		`UPDATE gray_rules SET version_id = ?, percentage = ?, whitelist = ?, platforms = ?,
             min_app_version = ?, priority = ?, enabled = ?, updated_at = ?
         WHERE id = ? AND package_id = ?`,
		versionID, input.Percentage, string(whitelist), string(platforms), input.MinAppVersion,
		input.Priority, boolInt(input.Enabled), timeText(now), id, pkg.ID)
	if err != nil {
		return GrayRule{}, err
	}
	affected, err := result.RowsAffected()
	if err != nil {
		return GrayRule{}, err
	}
	if affected == 0 {
		return GrayRule{}, ErrNotFound
	}
	return s.GrayRuleByID(ctx, pkg.Name, id)
}

func (s *Store) DeleteGrayRule(ctx context.Context, packageName, id string) error {
	pkg, err := s.PackageByName(ctx, packageName)
	if err != nil {
		return err
	}
	result, err := s.db.ExecContext(ctx,
		"DELETE FROM gray_rules WHERE id = ? AND package_id = ?", id, pkg.ID)
	if err != nil {
		return err
	}
	affected, err := result.RowsAffected()
	if err != nil {
		return err
	}
	if affected == 0 {
		return ErrNotFound
	}
	return nil
}

func (s *Store) GrayRuleByID(ctx context.Context, packageName, id string) (GrayRule, error) {
	row := s.db.QueryRowContext(ctx,
		`SELECT g.id, g.package_id, g.version_id, v.version, g.percentage, g.whitelist,
                g.platforms, g.min_app_version, g.priority, g.enabled, g.created_at, g.updated_at
         FROM gray_rules g
         JOIN packages p ON p.id = g.package_id
         JOIN versions v ON v.id = g.version_id
         WHERE p.name = ? AND g.id = ?`, packageName, id)
	value, err := scanGrayRule(row)
	if errors.Is(err, sql.ErrNoRows) {
		return GrayRule{}, ErrNotFound
	}
	return value, err
}

func (s *Store) ListGrayRules(ctx context.Context, packageName string) ([]GrayRule, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT g.id, g.package_id, g.version_id, v.version, g.percentage, g.whitelist,
                g.platforms, g.min_app_version, g.priority, g.enabled, g.created_at, g.updated_at
         FROM gray_rules g
         JOIN packages p ON p.id = g.package_id
         JOIN versions v ON v.id = g.version_id
         WHERE p.name = ?
         ORDER BY g.priority DESC, g.created_at ASC`, packageName)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var values []GrayRule
	for rows.Next() {
		value, err := scanGrayRule(rows)
		if err != nil {
			return nil, err
		}
		values = append(values, value)
	}
	return values, rows.Err()
}

func scanGrayRule(row rowScanner) (GrayRule, error) {
	var value GrayRule
	var whitelist, platforms, createdAt, updatedAt string
	var enabled int
	err := row.Scan(&value.ID, &value.PackageID, &value.VersionID, &value.Version, &value.Percentage,
		&whitelist, &platforms, &value.MinAppVersion, &value.Priority, &enabled, &createdAt, &updatedAt)
	if err != nil {
		return GrayRule{}, err
	}
	_ = json.Unmarshal([]byte(whitelist), &value.Whitelist)
	_ = json.Unmarshal([]byte(platforms), &value.Platforms)
	value.Enabled = enabled != 0
	value.CreatedAt = parseTime(createdAt)
	value.UpdatedAt = parseTime(updatedAt)
	return value, nil
}

func cleanStrings(values []string) []string {
	result := make([]string, 0, len(values))
	seen := make(map[string]struct{}, len(values))
	for _, value := range values {
		value = normalize(value)
		if value == "" {
			continue
		}
		if _, exists := seen[value]; exists {
			continue
		}
		seen[value] = struct{}{}
		result = append(result, value)
	}
	return result
}

func boolInt(value bool) int {
	if value {
		return 1
	}
	return 0
}
