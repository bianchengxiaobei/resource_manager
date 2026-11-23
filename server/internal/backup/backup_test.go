package backup

import (
	"context"
	"os"
	"path/filepath"
	"testing"

	"parcel/server/internal/blob"
	"parcel/server/internal/store"
)

func TestCreateBackup(t *testing.T) {
	base := t.TempDir()
	dbPath := filepath.Join(base, "parcel.db")
	blobRoot := filepath.Join(base, "blobs")
	st, err := store.Open(dbPath)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := st.CreatePackage(context.Background(), "game", ""); err != nil {
		t.Fatal(err)
	}
	if err := st.Close(); err != nil {
		t.Fatal(err)
	}
	blobs, err := blob.New(blobRoot)
	if err != nil {
		t.Fatal(err)
	}
	info, err := blobs.SaveBytes([]byte("blob"), 1024)
	if err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(base, "backup")
	manifest, err := Create(dbPath, blobRoot, out)
	if err != nil {
		t.Fatal(err)
	}
	if manifest.Database != "parcel.db" {
		t.Fatalf("manifest = %+v", manifest)
	}
	for _, name := range []string{"parcel.db", "backup.json", filepath.Join("blobs", info.SHA256)} {
		if _, err := os.Stat(filepath.Join(out, name)); err != nil {
			t.Fatalf("missing %s: %v", name, err)
		}
	}
	restoreDB := filepath.Join(base, "restored.db")
	restoreBlobs := filepath.Join(base, "restored-blobs")
	if err := Restore(out, restoreDB, restoreBlobs); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(restoreBlobs, info.SHA256)); err != nil {
		t.Fatalf("restored blob missing: %v", err)
	}
}
