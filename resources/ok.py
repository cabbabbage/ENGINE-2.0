from pathlib import Path
from PIL import Image

# Maximum size for the larger dimension
MAX_DIMENSION = 420

# Image file types to process
IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".bmp", ".tiff", ".tif", ".webp"}


def resize_image_in_place(image_path: Path) -> None:
    try:
        with Image.open(image_path) as img:
            width, height = img.size
            largest_dimension = max(width, height)

            # Skip if already smaller than or equal to 1080 in both dimensions
            if largest_dimension <= MAX_DIMENSION:
                return

            scale = MAX_DIMENSION / largest_dimension
            new_width = max(1, round(width * scale))
            new_height = max(1, round(height * scale))

            resized = img.resize((new_width, new_height), Image.LANCZOS)

            save_kwargs = {}
            if img.format == "JPEG":
                save_kwargs["quality"] = 95
                save_kwargs["optimize"] = True
            elif img.format == "PNG":
                save_kwargs["optimize"] = True

            resized.save(image_path, format=img.format, **save_kwargs)
            print(f"Resized: {image_path} -> {new_width}x{new_height}")

    except Exception as e:
        print(f"Failed: {image_path} ({e})")


def main() -> None:
    script_folder = Path(__file__).resolve().parent

    for path in script_folder.rglob("*"):
        if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS:
            resize_image_in_place(path)


if __name__ == "__main__":
    main()