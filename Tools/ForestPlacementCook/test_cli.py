import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def check_overview(tool, root, cook, header, output):
    result = cook(header + "0,21,8,-7,0,1\n1,21,3,-7,180,1\n")
    assert result.returncode == 0, result.stderr
    source = root / "canopy.png"
    canopy = b"\x00\xff\x00\x00\xff\x00\xff\x00\x00" * 2
    source.write_bytes(b"\x89PNG\r\n\x1a\n" +
                       png_chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 6, 0, 0, 0)) +
                       png_chunk(b"IDAT", zlib.compress(canopy)) + png_chunk(b"IEND", b""))
    mesh = root / "overview.glb"
    command = [tool, "--overview", output, source, mesh, "-1", "-1", "1", "1", "3", "16"]
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    glb = mesh.read_bytes()
    magic, version, size, json_size, kind = struct.unpack_from("<4sIII4s", glb)
    assert (magic, version, size, kind) == (b"glTF", 2, len(glb), b"JSON")
    asset = json.loads(glb[20:20 + json_size])
    assert asset["materials"][0]["alphaMode"] == "MASK"
    assert asset["materials"][0]["extras"] == {"luciaForestCanopyCoverage": True}
    primitive = asset["meshes"][0]["primitives"][0]
    payload = glb[28 + json_size:]

    def view(index):
        entry = asset["bufferViews"][index]
        return payload[entry["byteOffset"]:entry["byteOffset"] + entry["byteLength"]]

    positions = list(struct.iter_unpack("<3f", view(primitive["attributes"]["POSITION"])))
    assert len(positions) == 8 and all(19 < -x < 23 and -9 < z < -5 for x, y, z in positions)
    assert all(abs(y - 11) < 0.01 for x, y, z in positions[:4])
    assert all(abs(y - 6) < 0.01 for x, y, z in positions[4:])
    assert struct.unpack("<12I", view(primitive["indices"])) == (0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7)
    uvs = list(struct.iter_unpack("<2f", view(primitive["attributes"]["TEXCOORD_0"])))
    assert max(u for u, v in uvs[:4]) < min(u for u, v in uvs[4:])
    png = view(asset["images"][0]["bufferView"])
    assert png.startswith(b"\x89PNG\r\n\x1a\n") and struct.unpack_from(">II", png, 16) == (48, 24)
    corrupted = bytearray(output.read_bytes())
    corrupted[-1] ^= 1
    output.write_bytes(corrupted)
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode != 0 and "Could not verify placement page 1" in result.stderr
    assert mesh.read_bytes() == glb


def png_rgba(width, height, pixels):
    rows = b"".join(b"\x00" + pixels[row * width * 4:(row + 1) * width * 4] for row in range(height))
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            png_chunk(b"IDAT", zlib.compress(rows)) + png_chunk(b"IEND", b""))


def write_splat_tree(path):
    # Mesh 0: a masked leaf quad at y 4, x 0..2, facing +Y, with glTF material 1. Its texture cuts out
    # u > 0.5, which is glTF x > 1. Mesh 1: an opaque bark quad, x 0..1, y 0..3, facing -Z, material 0.
    bark = [(0, 0, 0), (1, 0, 0), (1, 3, 0), (0, 3, 0)], (0, 0, -1)
    leaves = [(0, 4, 0), (2, 4, 0), (2, 4, 1), (0, 4, 1)], (0, 1, 0)
    texture = png_rgba(2, 1, b"\x00\xff\x00\xff\x00\xff\x00\x00")
    blob, views, accessors, primitives = b"", [], [], []

    def add(data, **accessor):
        nonlocal blob
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        blob += data + b"\x00" * (-len(data) % 4)
        if accessor:
            accessors.append({"bufferView": len(views) - 1, **accessor})
        return len(accessors) - 1

    for material, (corners, normal) in ((1, leaves), (0, bark)):
        attributes = {
            "POSITION": add(b"".join(struct.pack("<3f", *c) for c in corners), componentType=5126, count=4,
                            type="VEC3", min=[min(c[i] for c in corners) for i in range(3)],
                            max=[max(c[i] for c in corners) for i in range(3)]),
            "NORMAL": add(struct.pack("<3f", *normal) * 4, componentType=5126, count=4, type="VEC3"),
            "TEXCOORD_0": add(b"".join(struct.pack("<2f", c[0] / 2, c[2]) for c in corners),
                              componentType=5126, count=4, type="VEC2"),
        }
        indices = add(struct.pack("<6H", 0, 1, 2, 0, 2, 3), componentType=5123, count=6, type="SCALAR")
        primitives.append({"attributes": attributes, "indices": indices, "material": material})
    add(texture)
    asset = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
             "meshes": [{"primitives": primitives}], "buffers": [{"byteLength": len(blob)}],
             "bufferViews": views, "accessors": accessors,
             "images": [{"bufferView": len(views) - 1, "mimeType": "image/png"}], "textures": [{"source": 0}],
             "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.25, 0.125, 1]}},
                           {"alphaMode": "MASK", "alphaCutoff": 0.5,
                            "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}]}
    text = json.dumps(asset).encode()
    text += b" " * (-len(text) % 4)
    path.parent.mkdir(parents=True)
    path.write_bytes(struct.pack("<4sII", b"glTF", 2, 28 + len(text) + len(blob)) +
                     struct.pack("<I4s", len(text), b"JSON") + text + struct.pack("<I4s", len(blob), b"BIN\x00") + blob)


def bake_splats(tool, assets, output, *materials):
    result = subprocess.run([tool, "--splats", assets, "Models/Tree.glb", output, *materials],
                            capture_output=True, text=True)
    if result.returncode != 0:
        return result, None
    data = output.read_bytes()
    magic, version, count, x, y, z, size, reserved = struct.unpack_from("<4sII4fI", data)
    assert (magic, version, reserved, len(data)) == (b"GEFS", 1, 0, 32 + 12 * count)
    assert abs(x + 2) < 1e-6 and y == 0 and z == 0 and abs(size - 4 / 19) < 1e-6
    # Grid X, Y, Z, coverage, R, G, B, normal X, normal Y, bend, leaf, zero.
    cells = list(struct.iter_unpack("<12B", data[32:]))
    assert all(cell[11] == 0 for cell in cells)
    return result, [(x + cell[0] * size, cell) for cell in cells]


def check_splats(tool, root):
    assets = root / "Assets"
    write_splat_tree(assets / "Models/Tree.glb")
    output = root / "tree.geforestsplats"
    result, cells = bake_splats(tool, assets, output)
    assert result.returncode == 0, result.stderr
    bark = [cell for x, cell in cells if cell[1] < 19]
    leaves = [(x, cell) for x, cell in cells if cell[1] == 19]
    # The engine model space mirrors glTF X: the bark spans x -1..0.
    assert bark and all(cell[10] == 0 and cell[4:7] == (128, 64, 32) for cell in bark)
    assert all(cell[0] >= 4 for cell in bark)
    assert all(90 <= cell[3] <= 110 for cell in bark if 0 < cell[1] < 14 and 5 < cell[0] < 9)
    # Cut-out texels at glTF x > 1 leave no cells. Leaf area counts 0.12 times; an upward leaf cell
    # then gets four times its coverage.
    assert leaves and all(cell[10] == 255 and cell[4:7] == (0, 255, 0) for x, cell in leaves)
    assert all(x > -1.1 for x, cell in leaves) and min(x for x, cell in leaves) < -0.9
    assert all(48 <= cell[3] <= 72 and cell[3] % 4 == 0 for x, cell in leaves if 0 < cell[2] < 4 and x > -0.8)

    material = assets / "Materials/Opaque.material"
    material.parent.mkdir()
    material.write_text(json.dumps({"schemaVersion": 3, "materialName": "Opaque", "alphaMode": "Opaque",
                                    "properties": {"baseColor": [1, 0.5, 0.25, 1]}}))
    result, cells = bake_splats(tool, assets, output, "0=Materials/Opaque.material")
    assert result.returncode == 0, result.stderr
    leaves = [(x, cell) for x, cell in cells if cell[1] == 19]
    assert all(cell[10] == 0 and cell[4:7] == (255, 128, 64) for x, cell in leaves)
    assert min(x for x, cell in leaves) < -1.9
    valid = output.read_bytes()
    for bad, message in (("2=Materials/Opaque.material", "no mesh 2"),
                         ("1=Materials/Missing.material", "Materials/Missing.material")):
        result, cells = bake_splats(tool, assets, output, bad)
        assert result.returncode != 0 and message in result.stderr, result.stderr
        assert output.read_bytes() == valid


def main():
    tool = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="forest-placement-cook-") as root:
        root = pathlib.Path(root)
        source = root / "trees.csv"
        output = root / "trees.geforest"
        header = "page,x,y,z,yawDegrees,scale\n"
        rows = "0,512000000,8,-7,0,0.75\n0,512000010,8,-6,90,1.25\n1,8,3,9,-90,1\n"

        def cook(text):
            source.write_text(text, encoding="utf-8")
            return subprocess.run([tool, source, output, "12"], capture_output=True, text=True)

        result = cook(header + rows)
        assert result.returncode == 0, result.stderr
        valid = output.read_bytes()
        assert struct.unpack_from("<4sIIIfIQ", valid) == (b"GEFP", 1, 2, 3, 12.0, 0, 168)
        page_format = "<3d6fQII"
        first = struct.unpack_from(page_format, valid, 40)
        second = struct.unpack_from(page_format, valid, 104)
        assert first[:3] == (512000000, 8, -7) and second[:3] == (8, 3, 9)
        assert first[-2:] == (0, 2) and second[-2:] == (2, 1)
        assert first[3] == first[4] == first[5] <= -15
        assert first[6] >= 25 and first[7] >= 15 and first[8] >= 16
        assert list(struct.iter_unpack("<3HBB", valid[168:])) == [
            (0, 0, 0, 0, 0), (1000, 100, 0, 64, 255), (0, 0, 0, 192, 128)
        ]
        for bad, message in (
            ("", "no trees"),
            ("0,0,0,0,0,1\n0,1,700,1,0,1\n", "655.35 meters"),
            ("1,0,0,0,0,1\n", "Line 2 has page 1"),
            ("0,0,0,0,0,1\n2,0,0,0,0,1\n", "Line 3 has page 2"),
            ("0,0,0,0,0,1\n0,800,0,0,0,1\n", "655.35 meters"),
            ("0,0,0,0,0,2\n", "Line 2 is not a valid placement"),
            ("0,0,nan,0,0,1\n", "Line 2 is not a valid placement"),
            ("0,0,0,0,0,1\n" * 145, "Line 146 is tree 145"),
        ):
            result = cook(header + bad)
            assert result.returncode != 0 and output.read_bytes() == valid, result.stderr
            assert message in result.stderr, result.stderr
        # Trees of one page may stand at different heights.
        result = cook(header + "0,0,2,0,0,1\n0, 1 ,7.5,1,0,1\n")
        assert result.returncode == 0, result.stderr
        sloped = output.read_bytes()
        page = struct.unpack_from(page_format, sloped, 40)
        assert page[1] == 2 and page[7] >= 5.5 + 15
        assert [tree[2] for tree in struct.iter_unpack("<3HBB", sloped[104:])] == [0, 550]
        result = cook(header + rows)
        assert result.returncode == 0 and output.read_bytes() == valid, result.stderr
        # Keep the input archive unchanged by default. The spatial mode moves whole pages
        # and keeps every packed placement attached to its original world-space page.
        source.write_text(header + "0,100,0,100,90,1\n0,101,0,100,180,1\n1,0,0,0,270,1\n",
                          encoding="utf-8")
        command = [tool, source, output, "12", "--spatial-order"]
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        ordered = output.read_bytes()
        assert struct.unpack_from(page_format, ordered, 40)[:3] == (0, 0, 0)
        assert struct.unpack_from(page_format, ordered, 40)[-2:] == (0, 1)
        assert struct.unpack_from(page_format, ordered, 104)[:3] == (100, 0, 100)
        assert struct.unpack_from(page_format, ordered, 104)[-2:] == (1, 2)
        assert list(struct.iter_unpack("<3HBB", ordered[168:])) == [
            (0, 0, 0, 192, 128), (0, 0, 0, 64, 128), (100, 0, 0, 128, 128)]
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode == 0 and output.read_bytes() == ordered, result.stderr
        check_overview(tool, root, cook, header, output)
        check_splats(tool, root)


if __name__ == "__main__":
    main()
