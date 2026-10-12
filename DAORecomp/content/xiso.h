#pragma once
// Reading files out of an Xbox 360 disc image (XDVDFS), as scripts/xiso.py does: setup
// shows the game's menu art from the player's disc image before it's installed.

namespace dao::content
{
    // Copies the file at `path` ('/'-separated, any case, e.g.
    // "packages/core/data/guiexport.erf") out of the disc image to `out`, if it's at
    // most maxSize bytes. False, with `error`, if it can't.
    bool ExtractIsoFile(const std::filesystem::path& iso, std::string_view path, const std::filesystem::path& out,
        uint64_t maxSize, std::string& error);
}
