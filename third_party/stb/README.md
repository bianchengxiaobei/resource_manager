# stb

Vendored single-header image decoder used by the texture importer.

- Upstream: https://github.com/nothings/stb
- `stb_image.h` and `stb_image_resize.h` are public domain / MIT (dual licensed).
- See the license block at the top of each header.
- Only `stb_image` is compiled into `parcel_stb`; the resize header is retained
  for future mip/filter work.
