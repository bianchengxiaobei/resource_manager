package store

import (
	"context"
	"database/sql"
	"errors"
	"time"
)

func (s *Store) BootstrapAdmin(ctx context.Context, username, passwordHash string) (bool, error) {
	var count int
	if err := s.db.QueryRowContext(ctx, "SELECT COUNT(1) FROM users").Scan(&count); err != nil {
		return false, err
	}
	if count > 0 {
		return false, nil
	}
	_, err := s.db.ExecContext(ctx,
		"INSERT INTO users(id, username, password_hash, role, created_at) VALUES(?, ?, ?, 'admin', ?)",
		NewID(), username, passwordHash, nowText())
	if err != nil {
		if isUniqueError(err) {
			return false, nil
		}
		return false, err
	}
	return true, nil
}

func (s *Store) CreateUser(ctx context.Context, username, passwordHash, role string) (User, error) {
	if role != "admin" && role != "operator" {
		return User{}, ErrInvalid
	}
	user := User{
		ID:           NewID(),
		Username:     username,
		PasswordHash: passwordHash,
		Role:         role,
		CreatedAt:    time.Now().UTC(),
	}
	_, err := s.db.ExecContext(ctx,
		"INSERT INTO users(id, username, password_hash, role, created_at) VALUES(?, ?, ?, ?, ?)",
		user.ID, user.Username, user.PasswordHash, user.Role, timeText(user.CreatedAt))
	if err != nil {
		if isUniqueError(err) {
			return User{}, ErrConflict
		}
		return User{}, err
	}
	return user, nil
}

func (s *Store) UserByUsername(ctx context.Context, username string) (User, error) {
	var user User
	var createdAt string
	err := s.db.QueryRowContext(ctx,
		"SELECT id, username, password_hash, role, created_at FROM users WHERE username = ?",
		username).Scan(&user.ID, &user.Username, &user.PasswordHash, &user.Role, &createdAt)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, ErrNotFound
	}
	if err != nil {
		return User{}, err
	}
	user.CreatedAt = parseTime(createdAt)
	return user, nil
}

func (s *Store) UserByID(ctx context.Context, id string) (User, error) {
	var user User
	var createdAt string
	err := s.db.QueryRowContext(ctx,
		"SELECT id, username, password_hash, role, created_at FROM users WHERE id = ?",
		id).Scan(&user.ID, &user.Username, &user.PasswordHash, &user.Role, &createdAt)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, ErrNotFound
	}
	if err != nil {
		return User{}, err
	}
	user.CreatedAt = parseTime(createdAt)
	return user, nil
}

func (s *Store) CreateSession(ctx context.Context, id, userID, csrf string, expires time.Time) error {
	_, err := s.db.ExecContext(ctx,
		"INSERT INTO sessions(id, user_id, csrf_token, expires_at, created_at) VALUES(?, ?, ?, ?, ?)",
		id, userID, csrf, timeText(expires), nowText())
	return err
}

func (s *Store) SessionByID(ctx context.Context, id string, now time.Time) (Session, User, error) {
	var session Session
	var user User
	var expiresAt, createdAt, userCreatedAt string
	err := s.db.QueryRowContext(ctx,
		`SELECT s.id, s.user_id, s.csrf_token, s.expires_at, s.created_at,
                u.id, u.username, u.password_hash, u.role, u.created_at
         FROM sessions s JOIN users u ON u.id = s.user_id
         WHERE s.id = ? AND s.expires_at > ?`,
		id, timeText(now)).Scan(
		&session.ID, &session.UserID, &session.CSRFToken, &expiresAt, &createdAt,
		&user.ID, &user.Username, &user.PasswordHash, &user.Role, &userCreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Session{}, User{}, ErrNotFound
	}
	if err != nil {
		return Session{}, User{}, err
	}
	session.ExpiresAt = parseTime(expiresAt)
	session.CreatedAt = parseTime(createdAt)
	user.CreatedAt = parseTime(userCreatedAt)
	return session, user, nil
}

func (s *Store) DeleteSession(ctx context.Context, id string) error {
	_, err := s.db.ExecContext(ctx, "DELETE FROM sessions WHERE id = ?", id)
	return err
}

func (s *Store) DeleteExpiredSessions(ctx context.Context, now time.Time) error {
	_, err := s.db.ExecContext(ctx, "DELETE FROM sessions WHERE expires_at <= ?", timeText(now))
	return err
}

func (s *Store) CreateAPIKey(ctx context.Context, userID, name, keyHash string, expires *time.Time) (string, error) {
	id := NewID()
	var expiresText any
	if expires != nil {
		expiresText = timeText(*expires)
	}
	_, err := s.db.ExecContext(ctx,
		"INSERT INTO api_keys(id, user_id, name, key_hash, created_at, expires_at) VALUES(?, ?, ?, ?, ?, ?)",
		id, userID, name, keyHash, nowText(), expiresText)
	if err != nil {
		if isUniqueError(err) {
			return "", ErrConflict
		}
		return "", err
	}
	return id, nil
}

func (s *Store) UserByAPIKeyHash(ctx context.Context, keyHash string, now time.Time) (User, error) {
	var user User
	var userCreatedAt string
	err := s.db.QueryRowContext(ctx,
		`SELECT u.id, u.username, u.password_hash, u.role, u.created_at
         FROM api_keys k JOIN users u ON u.id = k.user_id
         WHERE k.key_hash = ? AND (k.expires_at IS NULL OR k.expires_at > ?)`,
		keyHash, timeText(now)).Scan(&user.ID, &user.Username, &user.PasswordHash, &user.Role, &userCreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, ErrNotFound
	}
	if err != nil {
		return User{}, err
	}
	user.CreatedAt = parseTime(userCreatedAt)
	return user, nil
}

func (s *Store) SaveDeviceToken(ctx context.Context, playerID, tokenHash string, expires time.Time) error {
	_, err := s.db.ExecContext(ctx,
		"INSERT INTO device_tokens(id, player_id, token_hash, expires_at, created_at) VALUES(?, ?, ?, ?, ?)",
		NewID(), playerID, tokenHash, timeText(expires), nowText())
	return err
}
