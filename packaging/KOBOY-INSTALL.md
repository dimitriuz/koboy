# koboy — this archive is not empty

Everything is inside a folder named `.adds`, and a leading dot means
**hidden**: Linux file managers, macOS Finder and Windows Explorer all hide
it by default, so an archive window that looks empty is showing you exactly
what it should.

`.adds` is not koboy's choice. It is where every Kobo add-on lives — KOReader,
Plato, NickelMenu — because it is the one directory Kobo's own software leaves
alone. The path has to be `.adds/koboy/` or nothing finds it.

## Install

Extract this whole archive to the **root** of the drive your Kobo appears as
over USB — the folder holding `.kobo`, alongside your books. Your extractor
creates `.adds/koboy/` for you; you never have to see or open the hidden
folder yourself.

    unzip koboy-*.zip -d /run/media/you/KOBOeReader     # Linux
    unzip koboy-*.zip -d /Volumes/KOBOeReader           # macOS

On Windows, right-click the zip, "Extract All", and choose the Kobo drive.

Then eject, and put your ROMs in `.adds/koboy/roms/`. Full instructions,
including how to launch koboy, are in `.adds/koboy/README.md`.

## This file

It exists only so the archive does not look empty before you extract it.
Once koboy is installed you can delete it from your Kobo; nothing reads it.
