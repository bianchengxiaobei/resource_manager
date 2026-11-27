package blob

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

var (
	ErrTooLarge    = errors.New("blob exceeds size limit")
	ErrInvalidHash = errors.New("invalid SHA-256")
)

var hashPattern = regexp.MustCompile(`^[0-9a-f]{64}$`)

type Store struct {
	root string
	tmp  string
}

type Info struct {
	SHA256 string
	Size   int64
}

type Writer struct {
	store *Store
	file  *os.File
	hash  interface {
		Write([]byte) (int, error)
		Sum([]byte) []byte
	}
	size int64
}

func New(root string) (*Store, error) {
	if strings.TrimSpace(root) == "" {
		return nil, errors.New("blob root is empty")
	}
	tmp := filepath.Join(root, ".tmp")
	if err := os.MkdirAll(tmp, 0o755); err != nil {
		return nil, err
	}
	return &Store{root: root, tmp: tmp}, nil
}

func (s *Store) Root() string { return s.root }

func (s *Store) Begin() (*Writer, error) {
	file, err := os.CreateTemp(s.tmp, "upload-*")
	if err != nil {
		return nil, err
	}
	return &Writer{store: s, file: file, hash: sha256.New()}, nil
}

func (w *Writer) Write(value []byte) (int, error) {
	written, err := w.file.Write(value)
	if written > 0 {
		_, _ = w.hash.Write(value[:written])
		w.size += int64(written)
	}
	return written, err
}

func (w *Writer) Size() int64 { return w.size }

func (w *Writer) Commit(expected string) (Info, error) {
	sum := hex.EncodeToString(w.hash.Sum(nil))
	if expected != "" {
		expected = strings.ToLower(strings.TrimSpace(expected))
		if expected != sum {
			w.Abort()
			return Info{}, fmt.Errorf("blob SHA-256 mismatch: expected %s got %s", expected, sum)
		}
	}
	if err := w.file.Sync(); err != nil {
		w.Abort()
		return Info{}, err
	}
	if err := w.file.Close(); err != nil {
		_ = os.Remove(w.file.Name())
		return Info{}, err
	}
	destination := sPath(w.store.root, sum)
	if _, err := os.Stat(destination); err == nil {
		_ = os.Remove(w.file.Name())
		return Info{SHA256: sum, Size: w.size}, nil
	}
	if err := os.MkdirAll(filepath.Dir(destination), 0o755); err != nil {
		_ = os.Remove(w.file.Name())
		return Info{}, err
	}
	if err := os.Rename(w.file.Name(), destination); err != nil {
		if _, statErr := os.Stat(destination); statErr == nil {
			_ = os.Remove(w.file.Name())
			return Info{SHA256: sum, Size: w.size}, nil
		}
		_ = os.Remove(w.file.Name())
		return Info{}, err
	}
	return Info{SHA256: sum, Size: w.size}, nil
}

func (w *Writer) Abort() {
	if w.file == nil {
		return
	}
	name := w.file.Name()
	_ = w.file.Close()
	_ = os.Remove(name)
	w.file = nil
}

func (s *Store) Save(reader io.Reader, maxBytes int64) (Info, error) {
	writer, err := s.Begin()
	if err != nil {
		return Info{}, err
	}
	var source io.Reader = reader
	if maxBytes > 0 {
		source = io.LimitReader(reader, maxBytes+1)
	}
	written, err := io.Copy(writer, source)
	if err != nil {
		writer.Abort()
		return Info{}, err
	}
	if maxBytes > 0 && written > maxBytes {
		writer.Abort()
		return Info{}, ErrTooLarge
	}
	return writer.Commit("")
}

func (s *Store) SaveBytes(value []byte, maxBytes int64) (Info, error) {
	if maxBytes > 0 && int64(len(value)) > maxBytes {
		return Info{}, ErrTooLarge
	}
	writer, err := s.Begin()
	if err != nil {
		return Info{}, err
	}
	if _, err := writer.Write(value); err != nil {
		writer.Abort()
		return Info{}, err
	}
	return writer.Commit("")
}

func (s *Store) Open(sha string) (*os.File, error) {
	if !hashPattern.MatchString(sha) {
		return nil, ErrInvalidHash
	}
	return os.Open(sPath(s.root, sha))
}

func (s *Store) Stat(sha string) (os.FileInfo, error) {
	if !hashPattern.MatchString(sha) {
		return nil, ErrInvalidHash
	}
	return os.Stat(sPath(s.root, sha))
}

func (s *Store) Has(sha string) bool {
	info, err := s.Stat(sha)
	return err == nil && info.Mode().IsRegular()
}

func (s *Store) Remove(sha string) error {
	if !hashPattern.MatchString(sha) {
		return ErrInvalidHash
	}
	err := os.Remove(sPath(s.root, sha))
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	return err
}

func (s *Store) CleanTemp() error {
	entries, err := os.ReadDir(s.tmp)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		_ = os.Remove(filepath.Join(s.tmp, entry.Name()))
	}
	return nil
}

func sPath(root, sha string) string {
	return filepath.Join(root, sha)
}
