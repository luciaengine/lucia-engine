# Text test fixture provenance

These Google Noto font subsets exercise shaping and fallback in the Text tests.
They are test fixtures and are not included in Editor or Player distributions.
The complete SIL Open Font License notices and upstream copyright credits are
retained alongside the fixtures.

| Fixture | Source | License and credits |
|---|---|---|
| `NotoSans-Regular.ttf` | [Pinned upstream face](https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSans/NotoSans-Regular.ttf) | [Noto-OFL.txt](Noto-OFL.txt) |
| `NotoSansArabic-Regular.ttf` | [Pinned upstream face](https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSansArabic/NotoSansArabic-Regular.ttf) | [Noto-OFL.txt](Noto-OFL.txt) |
| `NotoSansHebrew-Regular.ttf` | [Pinned upstream face](https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSansHebrew/NotoSansHebrew-Regular.ttf) | [Noto-OFL.txt](Noto-OFL.txt) |
| `NotoSansDevanagari-Regular.ttf` | [Pinned upstream face](https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSansDevanagari/NotoSansDevanagari-Regular.ttf) | [Noto-OFL.txt](Noto-OFL.txt) |
| `NotoSansThai-Regular.ttf` | [Pinned upstream face](https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSansThai/NotoSansThai-Regular.ttf) | [Noto-OFL.txt](Noto-OFL.txt) |
| `NotoSansJP-Regular.otf` | [Pinned upstream face](https://raw.githubusercontent.com/notofonts/noto-cjk/f8d157532fbfaeda587e826d4cd5b21a49186f7c/Sans/SubsetOTF/JP/NotoSansJP-Regular.otf) | [NotoCJK-OFL.txt](NotoCJK-OFL.txt) |

[NotoSources.json](NotoSources.json) records the exact upstream source commits,
URLs and source-file SHA-256 values. Those hashes describe the upstream inputs,
not the generated subsets. The Noto Sans families use noto-fonts commit
`ffebf8c1ee449e544955a7e813c54f9b73848eac`; the Japanese fixture uses noto-cjk commit
`f8d157532fbfaeda587e826d4cd5b21a49186f7c`.

The Latin fixture covers Basic Latin, Latin-1, combining marks and punctuation.
The Arabic, Hebrew, Devanagari, Thai and Japanese fixtures retain Basic Latin
and their regression strings, with the layout features needed for shaping.
These small fixtures do not establish complete language coverage.

To extend a fixture, subset its pinned source again with the existing test
characters and the new code points, retaining all layout features:

```sh
pyftsubset <source> --unicodes=U+0020-007E,<test code points> --layout-features='*' --name-IDs='*' --notdef-outline --glyph-names --output-file=<file>
```

This is the canonical fixture provenance and supersedes the Arabic-only notes in
[arabic-test-font.html](arabic-test-font.html). `LICENSE.txt` retains the earlier
Arabic fixture notice; `Noto-OFL.txt` is the notice for the pinned Noto Sans sources.

## Changelog

- 2026-11-02: Consolidated the six shaping fixtures and their pinned source and license records.
