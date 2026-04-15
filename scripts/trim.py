from __future__ import annotations

from pathlib import Path
from typing import Iterable, Optional, Tuple

from PIL import Image

# Supported image types in the same folder as this script
SUPPORTED_EXTS = {".png", ".webp", ".tga", ".bmp", ".gif", ".tif", ".tiff"}


def find_alpha_bounds(img: Image.Image) -> Optional[Tuple[int, int, int, int]]:
    """
    Return the bounding box of non-transparent pixels as:
    (left, top, right, bottom)

    right and bottom are exclusive, matching PIL bbox behavior.

    Returns None if the image has no visible alpha pixels.
    """
    rgba = img.convert("RGBA")
    alpha = rgba.getchannel("A")
    return alpha.getbbox()


def iter_images(folder: Path) -> Iterable[Path]:
    """
    Yield supported image files in the folder, excluding this script itself.
    """
    script_path = Path(__file__).resolve()
    for path in sorted(folder.iterdir()):
        if not path.is_file():
            continue
        if path.resolve() == script_path:
            continue
        if path.suffix.lower() in SUPPORTED_EXTS:
            yield path


def analyze_folder(folder: Path) -> Tuple[int, int, list[tuple[Path, Optional[Tuple[int, int, int, int]]]]]:
    """
    Analyze all images and return:
    - shared_width
    - shared_height
    - list of (path, bbox)
    """
    results: list[tuple[Path, Optional[Tuple[int, int, int, int]]]] = []

    max_content_width = 0
    max_content_height = 0

    for image_path in iter_images(folder):
        with Image.open(image_path) as img:
            bbox = find_alpha_bounds(img)
            results.append((image_path, bbox))

            if bbox is None:
                continue

            left, top, right, bottom = bbox
            content_width = right - left
            content_height = bottom - top

            max_content_width = max(max_content_width, content_width)
            max_content_height = max(max_content_height, content_height)

    return max_content_width, max_content_height, results


def recrop_to_shared_bounds(
    image_path: Path,
    bbox: Optional[Tuple[int, int, int, int]],
    out_width: int,
    out_height: int,
    output_folder: Path,
) -> None:
    """
    Crop the visible content from the source image, then place it onto a new
    transparent canvas of shared size.

    The content is horizontally centered and vertically bottom-aligned.
    """
    with Image.open(image_path) as img:
        rgba = img.convert("RGBA")

        # If fully transparent, just create an empty shared-size canvas
        if bbox is None:
            canvas = Image.new("RGBA", (out_width, out_height), (0, 0, 0, 0))
            out_path = output_folder / image_path.name
            canvas.save(out_path)
            return

        content = rgba.crop(bbox)
        content_width, content_height = content.size

        canvas = Image.new("RGBA", (out_width, out_height), (0, 0, 0, 0))

        # Center horizontally, align bottom vertically
        paste_x = (out_width - content_width) // 2
        paste_y = out_height - content_height

        canvas.alpha_composite(content, (paste_x, paste_y))

        out_path = output_folder / image_path.name
        canvas.save(out_path)


def main() -> None:
    script_folder = Path(__file__).resolve().parent
    output_folder = script_folder / "cropped_output"
    output_folder.mkdir(exist_ok=True)

    shared_width, shared_height, results = analyze_folder(script_folder)

    if not results:
        print("No supported image files found in the script folder.")
        return

    if shared_width <= 0 or shared_height <= 0:
        print("No images with visible alpha content were found.")
        print("Creating blank shared-size outputs is not possible because all images are fully transparent.")
        return

    print(f"Shared crop size: {shared_width}x{shared_height}")
    print(f"Found {len(results)} image(s).")
    print()

    for image_path, bbox in results:
        print(f"{image_path.name}: bbox={bbox}")
        recrop_to_shared_bounds(
            image_path=image_path,
            bbox=bbox,
            out_width=shared_width,
            out_height=shared_height,
            output_folder=output_folder,
        )

    print()
    print(f"Done. Cropped images saved to: {output_folder}")


if __name__ == "__main__":
    main()