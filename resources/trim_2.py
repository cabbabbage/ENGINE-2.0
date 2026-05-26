#!/usr/bin/env python3
from __future__ import annotations

import shutil
from pathlib import Path
from typing import Iterable, Optional

from PIL import Image

SUPPORTED_EXTS = {".png", ".webp", ".tga", ".bmp", ".gif", ".tif", ".tiff"}


def find_alpha_bounds(img: Image.Image) -> Optional[tuple[int, int, int, int]]:
    """
    Return the bounding box of non-transparent pixels as:
    (left, top, right, bottom)

    right and bottom are exclusive, matching PIL bbox behavior.

    Returns None if the image has no visible alpha pixels.
    """
    rgba = img.convert("RGBA")
    alpha = rgba.getchannel("A")
    return alpha.getbbox()


def bottom_transparent_rows(img_rgba: Image.Image) -> int:
    """
    Return the number of fully transparent rows at the bottom of the image.
    Fully transparent images return 0 so they are left unchanged.
    """
    alpha = img_rgba.getchannel("A")
    bbox = alpha.getbbox()

    if bbox is None:
        return 0

    h = img_rgba.height
    lower_exclusive = bbox[3]
    shift = h - lower_exclusive

    return shift if shift > 0 else 0


def shift_down_image(img_rgba: Image.Image) -> tuple[Image.Image, bool]:
    """
    Move fully transparent bottom rows to the top.
    Returns the shifted image and whether it changed.
    """
    shift = bottom_transparent_rows(img_rgba)

    if shift <= 0:
        return img_rgba, False

    w, h = img_rgba.size
    new_img = Image.new("RGBA", (w, h), (0, 0, 0, 0))

    upper_part = img_rgba.crop((0, 0, w, h - shift))
    new_img.paste(upper_part, (0, shift))

    return new_img, True


def shift_down_in_place(image_path: Path) -> bool:
    """
    Shift one image downward in place before crop analysis.
    Returns True if changed.
    """
    with Image.open(image_path) as img:
        rgba = img.convert("RGBA")
        shifted, changed = shift_down_image(rgba)

        if changed:
            shifted.save(image_path)

        return changed


def iter_asset_folders(assets_folder: Path) -> Iterable[Path]:
    """
    Yield each asset folder directly inside the assets folder.
    """
    for path in sorted(assets_folder.iterdir()):
        if path.is_dir():
            yield path


def iter_animation_folders(asset_folder: Path) -> Iterable[Path]:
    """
    Yield animation folders directly inside an asset folder.
    """
    for path in sorted(asset_folder.iterdir()):
        if path.is_dir():
            yield path


def iter_images_in_folder(folder: Path) -> Iterable[Path]:
    """
    Yield supported image files directly inside a folder.
    """
    for path in sorted(folder.iterdir()):
        if path.is_file() and path.suffix.lower() in SUPPORTED_EXTS:
            yield path


def iter_images_for_asset(asset_folder: Path) -> Iterable[Path]:
    """
    Yield all supported image files found inside all animation folders
    for a single asset.
    """
    for animation_folder in iter_animation_folders(asset_folder):
        for image_path in iter_images_in_folder(animation_folder):
            yield image_path


def shift_down_asset_images(asset_folder: Path) -> tuple[int, int]:
    """
    Shift every image for one asset before crop calculation.

    Returns:
    - images_seen
    - images_shifted
    """
    images_seen = 0
    images_shifted = 0

    for image_path in iter_images_for_asset(asset_folder):
        images_seen += 1

        if shift_down_in_place(image_path):
            images_shifted += 1

    return images_seen, images_shifted


def analyze_asset(
    asset_folder: Path,
) -> tuple[int, int, list[tuple[Path, Optional[tuple[int, int, int, int]]]]]:
    """
    Analyze every shifted image across every animation folder for one asset.

    Returns:
    - shared_width
    - shared_height
    - list of (image_path, bbox)
    """
    results: list[tuple[Path, Optional[tuple[int, int, int, int]]]] = []
    max_content_width = 0
    max_content_height = 0

    for image_path in iter_images_for_asset(asset_folder):
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


def recrop_in_place(
    image_path: Path,
    bbox: Optional[tuple[int, int, int, int]],
    out_width: int,
    out_height: int,
) -> None:
    """
    Crop visible content from the source image, then place it onto a new
    transparent canvas of shared size.

    The content is horizontally centered and vertically bottom-aligned.

    The result replaces the original image in place.
    """
    with Image.open(image_path) as img:
        rgba = img.convert("RGBA")

        if bbox is None:
            canvas = Image.new("RGBA", (out_width, out_height), (0, 0, 0, 0))
            canvas.save(image_path)
            return

        content = rgba.crop(bbox)
        content_width, content_height = content.size

        canvas = Image.new("RGBA", (out_width, out_height), (0, 0, 0, 0))

        paste_x = (out_width - content_width) // 2
        paste_y = out_height - content_height

        canvas.alpha_composite(content, (paste_x, paste_y))
        canvas.save(image_path)


def backup_assets_folder(script_folder: Path, assets_folder: Path) -> Path:
    """
    Create a full backup copy of the assets folder in the directory above
    where this script lives.

    The copy is named exactly: assets_coppy
    """
    backup_parent = script_folder.parent
    backup_folder = backup_parent / "assets_coppy"

    if backup_folder.exists():
        if backup_folder.is_dir():
            shutil.rmtree(backup_folder)
        else:
            backup_folder.unlink()

    shutil.copytree(assets_folder, backup_folder)
    return backup_folder


def main() -> None:
    script_folder = Path(__file__).resolve().parent
    assets_folder = script_folder / "assets"

    if not assets_folder.exists() or not assets_folder.is_dir():
        print(f'Could not find assets folder at: "{assets_folder}"')
        return

    print("Creating backup copy before modifying originals...")
    backup_folder = backup_assets_folder(script_folder, assets_folder)
    print(f'Backup created at: "{backup_folder}"')
    print()

    asset_folders = list(iter_asset_folders(assets_folder))
    if not asset_folders:
        print("No asset folders found inside the assets folder.")
        return

    total_assets_processed = 0
    total_images_processed = 0
    total_images_shifted = 0

    for asset_folder in asset_folders:
        print(f"[ASSET] {asset_folder.name}")

        images_seen, images_shifted = shift_down_asset_images(asset_folder)
        total_images_shifted += images_shifted

        if images_seen <= 0:
            print("  [SKIP] no supported images found in animation folders.")
            print()
            continue

        print(f"  Shifted before crop analysis: {images_shifted}/{images_seen}")

        shared_width, shared_height, results = analyze_asset(asset_folder)

        if not results:
            print("  [SKIP] no supported images found in animation folders.")
            print()
            continue

        if shared_width <= 0 or shared_height <= 0:
            print("  [SKIP] all images were fully transparent.")
            print()
            continue

        print(f"  Shared crop size: {shared_width}x{shared_height}")
        print(f"  Images found: {len(results)}")

        for image_path, bbox in results:
            print(f"    {image_path.relative_to(assets_folder)}: bbox={bbox}")
            recrop_in_place(
                image_path=image_path,
                bbox=bbox,
                out_width=shared_width,
                out_height=shared_height,
            )

        print()
        total_assets_processed += 1
        total_images_processed += len(results)

    print("Done.")
    print(f"Assets processed: {total_assets_processed}")
    print(f"Images processed: {total_images_processed}")
    print(f"Images shifted before crop: {total_images_shifted}")
    print(f'Backup copy: "{backup_folder}"')
    print(f'Original shifted and cropped in place: "{assets_folder}"')


if __name__ == "__main__":
    main()