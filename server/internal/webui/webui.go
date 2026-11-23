package webui

import (
	"embed"
	"io/fs"
)

//go:embed dist
var files embed.FS

func FS() fs.FS {
	value, err := fs.Sub(files, "dist")
	if err != nil {
		return nil
	}
	return value
}
