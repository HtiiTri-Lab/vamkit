# lang

The words the user sees, per language: the launch panel and the in-game menus (the graphics tab, Hair Budget, Display
Lighting, the path tracer's state line).

## How it works

- **English is in the code.** A plugin writes `Lang.T("Render scale")`, the launch panel `tr("Launch VaM")`. With
  values: `Lang.F("Gloss · {0}", name)`, `tr("{0} upscaling", model)`.
- **A language is one table**, `<code>.json` in this folder, installed to `<VaM>/vamkit/lang/`: a flat JSON object,
  English text -> that language's text, plus `"@name"`, the language's own name. A text the table lacks is shown in
  English.
- **Which language**: the launch panel's Language row writes `<VaM>/vamkit/launcher/profiles/language.json`
  (`{"language": "zh"}`). Without a choice, Windows' display language is followed (the list of primary languages in
  `VamkitLang.cs` and `lang.py`), English when there is no table for it. The panel changes at once; the in-game menus
  read the choice when VaM starts.
- **Context.** One English word that needs two words in another language carries a context after `##`:
  `Lang.T("Off")` is a switch's "off", `Lang.T("Off##choice")` the first of several choices. The context is never
  shown.
- **Texts kept as data** are marked where they are written, `Lang.N("Soft beauty")` / `mark("Vanilla")`, and
  translated where they are shown, `Lang.T(style.Label)`.

## Files

| File | Holds |
|---|---|
| `VamkitLang.cs` | `Lang` for the plugins: `T`, `F`, `N`, `Code`; the table reader. Compiled into each plugin that names it in `shared` (`manifest/components.toml`); no dependency but `System` |
| `../../render/launcher/lang.py` | The same for the launch panel: `tr`, `mark`, `languages`, `choose` |
| `zh.json` | Chinese |
| `tests/` | `LangTest.cs` (the reader, the choice, the fall-back, contexts), `lang_test.py` (the panel's side, and that both sides map Windows' languages alike) |

## Adding a language

```
python tools/check_lang.py --new ja      # writes ja.json with every text still in English
```

Translate the values, set `"@name"`, add the file to this component's `installs` in `manifest/components.toml`, and
when Windows should pick it by itself add its primary language id to `Windows()` in `VamkitLang.cs` and to `PRIMARY`
in `lang.py`. `python tools/check_lang.py` (also run by `vamkit test lang`) lists texts a table lacks, lines no code
asks for, and placeholders that differ.

## Adding or changing a text

Write the English in the code through `Lang.T` / `tr`, as one string literal (the checker reads literals), then add
its line to every table. Keep `{0}`, `{1}` in the same number in both.

The in-game graphics tab does not scroll: a label has about 16 Latin letters per line and two lines, a choice shares
the row with its neighbours and may take two lines too (`VaMVRMenu.Menu.cs` has the sizes; `LayoutAudit` there
reports what does not fit while the tab is shown). Prefer a shorter word to a smaller font.
