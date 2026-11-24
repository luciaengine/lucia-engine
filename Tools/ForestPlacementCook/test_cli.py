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
    result = cook(header + "0,1000000000.25,0,-1000000000.75,0,1\n1,-1000000000.25,0,1000000000.75,0,1\n")
    assert result.returncode == 0, result.stderr
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    glb = mesh.read_bytes()
    json_size, = struct.unpack_from("<I", glb, 12)
    asset = json.loads(glb[20:20 + json_size])
    payload = glb[28 + json_size:]
    positions = list(struct.iter_unpack("<3f", view(primitive["attributes"]["POSITION"])))
    assert asset["meshes"][0]["extras"]["luciaForestSectorLocal"] is True
    for page, (x, z) in enumerate(((1000000000.25, -1000000000.75), (-1000000000.25, 1000000000.75))):
        origin_x = ((x + 512) // 1024) * 1024
        origin_z = ((z + 512) // 1024) * 1024
        quad = positions[4 * page:4 * page + 4]
        xs = [origin_x - vx for vx, _, _ in quad]
        zs = [origin_z + vz for _, _, vz in quad]
        assert abs((min(xs) + max(xs)) / 2 - x) < 0.001 and 2.82 < max(xs) - min(xs) < 2.84
        assert abs((min(zs) + max(zs)) / 2 - z) < 0.001 and 2.82 < max(zs) - min(zs) < 2.84
    corrupted = bytearray(output.read_bytes())
    corrupted[-1] ^= 1
    output.write_bytes(corrupted)
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode != 0 and "Could not verify placement page 1" in result.stderr
    assert mesh.read_bytes() == glb


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


if __name__ == "__main__":
    main()
