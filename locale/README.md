# Translating S-MU2000

The texts of the windows, menus and tooltips live in this folder, one folder per
language:

```
locale/
  languages.json        the languages, in the order the program lists them
  en/                   English   (complete; every other language falls back to it)
  ja/                   Japanese  (complete)
  <your language>/      panel.json, menus.json, editor.json, voice.json, boards.json,
                        master.json, list.json, player.json, sampling.json
```

Each file is a list of `"key": "text"` pairs. You translate the text on the
right and leave the key on the left alone:

```jsonc
{
	// Bottom strip tabs (panel.cpp)
	"tab_panel": "Panel",
	"tab_editor": "Editor",
```

Lines starting with `//` are notes: they say where a text appears or what its
placeholders mean. You can add your own.

## Trying a translation without building anything

You do not need a compiler to see your texts in the program.

1. Download S-MU2000 and start it once.
2. Open its settings folder:
   - Windows: `%LOCALAPPDATA%\S-MU2000`
   - macOS: `~/Library/Application Support/S-MU2000`
   - Linux: `~/.local/share/S-MU2000`
3. Make a folder `locale` in it, and inside that a folder `en`.
4. Copy the `.json` files from `locale/en/` of this repository into it.
5. Change some texts, save, and start S-MU2000 again (in English: `--lang en`,
   or pick English in the program). Your texts are shown instead of the built-in ones.

The files in the settings folder are laid over a language the program already
has, which is why the steps above borrow `en` for a language that is not in the
program yet. Once your language has been added (below) its own folder name works too.

If something is wrong with a file the program says so on its console (stderr)
and carries on with the built-in text: a key it does not know, a line it cannot
read, or a text whose placeholders were changed.

## The rules

- **Keep the placeholders.** `%d`, `%s`, `%.2f` and friends are filled in by the
  program. A translation must have the same ones in the same order (`%d of %s`
  cannot become `%s of %d`). `%%` is a literal percent sign. Getting this wrong
  would crash the program, so both the build and the program refuse such a text.
- `\n` is a line break; keep the ones that are there unless your text needs a
  different split. Write a double quote as `\"`.
- Keys ending in `_fmt` are the ones with placeholders. Keys ending in `_tip` or
  `_hint` are tooltips, where the first line is a heading.
- The files are UTF-8. Any text editor will do; Notepad is fine.
- **A translation can be partial.** Delete the keys (or whole files) you have not
  translated: those texts show in English. Do not leave English text in your
  files for things you have not looked at, or nobody can tell what is done.
- Names printed on the MU2000 itself stay as they are: the buttons (`PLAY`,
  `EDIT`, `UTIL`), voice names, effect type names (`HALL 1`), parameter names in
  the XG tables. So does what the LCD shows.

## Adding a language

1. Pick the code: the two-letter language code, lowercase (`pl`, `de`, `fr`;
   `pt_br` if a region is needed).
2. If you have Python: `python tools/locale_tool.py new pl "Polski"` makes
   `locale/pl/` with every text in English and adds the language to
   `languages.json`. Without Python: copy `locale/en/` to `locale/pl/` and add
   `{"code": "pl", "name": "Polski"}` to `languages.json` by hand.
3. Translate. Delete what you leave for later.
4. Open a pull request with your `locale/pl/` folder and the `languages.json`
   change. If you can, run `python tools/locale_tool.py gen` first and include
   the files it writes under `src/ui/`; if you cannot, say so and a maintainer
   will do it.

Updating an existing language is the same without step 1 and 2.

## What is not here yet

Three kinds of text are still inside the source code and show in English (or
Japanese) whatever language is chosen. They are being moved here bit by bit:

- the long explanations in the hint bar of the list and voice windows
  (`HELP` in `src/ui/xg_ui.cpp`, the effect descriptions in `src/ui/fx_help.cpp`)
- what the command-line tools print on the console
- a few window titles

## For developers

`tools/locale_tool.py gen` turns these folders into the tables the program is
built with (`src/ui/texts_<code>.h`, `lang_list.inc`, `texts_tables.inc`,
`texts_fields.inc`). Those generated files are committed, so building does not
need Python, and `tools/check_texts.py` (run by `make test` and CI) fails when
they are not what `locale/` produces.

A new text is three things: a member in `struct ui_texts` (`src/ui/texts.h`,
with a comment saying where it shows, which becomes the note in every language's
file), its line in `locale/en` and `locale/ja`, and `gen`. In the code it is
`UI_TEXT(key, "the English text")`; the checker keeps that English copy equal to
`locale/en`.

If a branch edited `src/ui/texts_en.h` / `texts_ja.h` directly (older pull
requests do), `python tools/locale_tool.py export` writes those edits back into
`locale/`.
