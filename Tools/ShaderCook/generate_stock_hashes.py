"""Record the exact stock GLSL files available to a native renderer build."""
import argparse
from pathlib import Path


def shader_hash(data: bytes) -> int:
    # Matches HashShaderBytes and the shader cache's historical offset basis.
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    files = sorted(args.root.rglob("*.glsl"), key=lambda path: path.relative_to(args.root).as_posix())
    rows = [
        f'    {{"{path.relative_to(args.root).as_posix()}", 0x{shader_hash(path.read_bytes()):016x}ull}},'
        for path in files
    ]
    content = "\n".join(rows) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != content:
        temporary = args.output.with_suffix(".tmp")
        temporary.write_text(content)
        temporary.replace(args.output)


if __name__ == "__main__":
    main()
