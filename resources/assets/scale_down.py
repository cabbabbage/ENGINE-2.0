from pathlib import Path
from PIL import Image

# Image extensions to process
IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".tiff", ".tif"}

# Start from the folder this script lives in
ROOT = Path(__file__).resolve().parent


def resize_image(path: Path) -> None:
    try:
        with Image.open(path) as img:
            width, height = img.size
            new_size = (max(1, width // 2), max(1, height // 2))

            resized = img.resize(new_size, Image.LANCZOS)

            # Preserve format when possible
            save_kwargs = {}
            if img.format == "JPEG":
                save_kwargs["quality"] = 95
                save_kwargs["optimize"] = True
            elif img.format == "PNG":
                save_kwargs["optimize"] = True

            resized.save(path, format=img.format, **save_kwargs)
            print(f"Resized: {path}")
    except Exception as e:
        print(f"Failed: {path} -> {e}")


def main() -> None:
    for path in ROOT.rglob("*"):
        if path.is_file() and path.suffix.lower() in IMAGE_EXTS:
            resize_image(path)


if __name__ == "__main__":
    main()