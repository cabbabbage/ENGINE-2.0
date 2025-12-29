#!/usr/bin/env python3
"""
Recursively shifts PNG content downward by moving fully transparent bottom rows to the top.

Behavior (no args, overwrite by default):
- Walk all subfolders starting from the folder this script is in
- For each .png:
  - Find how many fully transparent rows exist at the bottom
  - Remove that transparent strip from the bottom
  - Add the same height transparent strip to the top
  - Save back over the original file

Notes:
- Images are converted to RGBA if needed.
- If an image is fully transparent, it is left unchanged.
"""

import os
from pathlib import Path
from PIL import Image


def bottom_transparent_rows(img_rgba: Image.Image) -> int:
    """
    Returns the number of fully transparent rows at the bottom of the image.
    If the image has no non-transparent pixels, returns 0 (leave unchanged).
    """
    alpha = img_rgba.getchannel("A")
    bbox = alpha.getbbox()  # bbox of alpha > 0, lower is exclusive
    if bbox is None:
        return 0
    h = img_rgba.height
    lower_exclusive = bbox[3]
    shift = h - lower_exclusive
    return shift if shift > 0 else 0


def shift_down_in_place(path: Path) -> bool:
    """
    Overwrites the PNG at `path` if it has transparent rows at the bottom.
    Returns True if changed, False otherwise.
    """
    with Image.open(path) as im:
        img = im.convert("RGBA")
        shift = bottom_transparent_rows(img)

        if shift <= 0:
            return False

        w, h = img.size
        new_img = Image.new("RGBA", (w, h), (0, 0, 0, 0))

        # Crop off bottom `shift` rows (known to be fully transparent)
        upper_part = img.crop((0, 0, w, h - shift))

        # Paste down by `shift`, leaving transparent padding at the top
        new_img.paste(upper_part, (0, shift))

        # Overwrite original
        new_img.save(path)

        return True


def main() -> None:
    root = Path(__file__).resolve().parent
    scanned = 0
    changed = 0
    errors = 0

    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            if not name.lower().endswith(".png"):
                continue

            path = Path(dirpath) / name
            scanned += 1

            try:
                if shift_down_in_place(path):
                    changed += 1
                    print(f"updated: {path}")
                else:
                    print(f"skip:    {path}")
            except Exception as e:
                errors += 1
                print(f"error:   {path} ({e})")

    print(f"done. scanned={scanned} changed={changed} errors={errors} root={root}")


if __name__ == "__main__":
    main()
