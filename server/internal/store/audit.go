package store

import (
	"context"
	"database/sql"
	"errors"
)

type AuditInput struct {
	UserID    string
	Action    string
	Target    string
	Detail    string
	RequestID string
}

func (s *Store) Audit(ctx context.Context, input AuditInput) error {
	if input.Action == "" || input.Target == "" {
		return ErrInvalid
	}
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO audit_logs(id, user_id, action, target, detail, request_id, created_at)
         VALUES(?, NULLIF(?, ''), ?, ?, ?, ?, ?)`,
		NewID(), input.UserID, input.Action, input.Target, input.Detail, input.RequestID, nowText())
	return err
}

func (s *Store) ListAudit(ctx context.Context, limit, offset int) ([]AuditEntry, int, error) {
	if limit <= 0 || limit > 200 {
		limit = 50
	}
	if offset < 0 {
		offset = 0
	}
	var total int
	if err := s.db.QueryRowContext(ctx, "SELECT COUNT(1) FROM audit_logs").Scan(&total); err != nil {
		return nil, 0, err
	}
	rows, err := s.db.QueryContext(ctx,
		`SELECT a.id, COALESCE(a.user_id, ''), COALESCE(u.username, ''), a.action, a.target,
                a.detail, a.request_id, a.created_at
         FROM audit_logs a LEFT JOIN users u ON u.id = a.user_id
         ORDER BY a.created_at DESC, a.id DESC LIMIT ? OFFSET ?`, limit, offset)
	if err != nil {
		return nil, 0, err
	}
	defer rows.Close()
	var values []AuditEntry
	for rows.Next() {
		var value AuditEntry
		var createdAt string
		if err := rows.Scan(&value.ID, &value.UserID, &value.Username, &value.Action,
			&value.Target, &value.Detail, &value.RequestID, &createdAt); err != nil {
			return nil, 0, err
		}
		value.CreatedAt = parseTime(createdAt)
		values = append(values, value)
	}
	return values, total, rows.Err()
}

func (s *Store) AuditEntryByID(ctx context.Context, id string) (AuditEntry, error) {
	var value AuditEntry
	var createdAt string
	err := s.db.QueryRowContext(ctx,
		`SELECT a.id, COALESCE(a.user_id, ''), COALESCE(u.username, ''), a.action, a.target,
                a.detail, a.request_id, a.created_at
         FROM audit_logs a LEFT JOIN users u ON u.id = a.user_id WHERE a.id = ?`, id).
		Scan(&value.ID, &value.UserID, &value.Username, &value.Action, &value.Target,
			&value.Detail, &value.RequestID, &createdAt)
	if errors.Is(err, sql.ErrNoRows) {
		return AuditEntry{}, ErrNotFound
	}
	if err != nil {
		return AuditEntry{}, err
	}
	value.CreatedAt = parseTime(createdAt)
	return value, nil
}
