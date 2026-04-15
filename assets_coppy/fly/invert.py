from pathlib import Path
from PIL import Image


def invert_png_in_place(png_path: Path) -> None:
    with Image.open(png_path) as img:
        img = img.convert("RGBA")
        pixels = img.load()

        width, height = img.size
        for y in range(height):
            for x in range(width):
                r, g, b, a = pixels[x, y]
                pixels[x, y] = (255 - r, 255 - g, 255 - b, a)

        img.save(png_path)


def main() -> None:
    root = Path(__file__).resolve().parent

    for png_path in root.rglob("*.png"):
        try:
            invert_png_in_place(png_path)
            print(f"Inverted: {png_path}")
        except Exception as e:
            print(f"Failed: {png_path} -> {e}")


if __name__ == "__main__":
    main()