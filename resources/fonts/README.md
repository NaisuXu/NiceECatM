# Embedded font

`workbench_font.cpp` embeds a compressed TrueType subset of Noto Sans SC, licensed under the adjacent SIL Open Font License. It includes 7,278 glyphs: GB2312 Chinese characters, Latin, UI punctuation and a few symbols. Fonts are part of the executable; users do not install fonts or Python packages.

The font was instantiated at weight 400 from [Google Fonts Noto Sans SC](https://github.com/google/fonts/tree/main/ofl/notosanssc), subset using fontTools, and packed with Dear ImGui's `misc/fonts/binary_to_compressed_c.cpp -base85`. These tools are needed only to regenerate the resource. Uncommon characters outside the subset may display as replacement glyphs; original UTF-8 device data is retained.

The embedded subset uses Noto's typographic ascent/descent (880 / −120) in the `hhea` table, keeping the requested UI text size consistent with its em height. This avoids unusually small glyphs in the compact workbench without increasing control heights.

The script editor uses the bundled Cousine Regular font for a clearer monospaced code layout. `cousine_font.cpp` is the compressed, embedded form used by the executable; `Cousine-Regular.ttf` is kept beside it as the source font for regeneration. This bundled Version 1.00 identifies the SIL Open Font License 1.1 in its embedded name records; its Google 2010 copyright notice and license are retained in `Cousine-OFL.txt` and installed with the application.
