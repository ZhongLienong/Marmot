# Image (Marmot package)

Simple image read/write bindings for Marmot using Rust and the image crate.

## API
- ReadInfo(path: Text) -> Array<Int>
  Returns [width, height].
- ReadRGBA(path: Text) -> Array<Int>
  Returns RGBA bytes (width * height * 4) as integer values 0..255.
- WriteRGBA(path: Text, width: Int, height: Int, data: Array<Int>) -> Bool
  Writes RGBA bytes to the target path. The file extension selects the format.

## Build (Windows)
1) cd native
2) cargo build --release
3) Copy native/target/release/marmot_image.dll to lib/windows/x64/marmot_image.dll

## Use
Set MARMOT_PATH to include this package and MarmotPrelude, then import it.

Example:
```marmot
import { <Image> }

// Read
def info = Image::ReadInfo("input.png");
def width = info[0];
def height = info[1];
def pixels = Image::ReadRGBA("input.png");

// Write
def ok = Image::WriteRGBA("output.png", width, height, pixels);
```
