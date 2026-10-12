"""set_config.py <user config> <dest> [section.key=value ...]

Writes the user's config.toml to <dest> for a test run: the launcher hidden, the intro
logos skipped, and each section.key=value set (replaced if present, else added under
its section, which is added if missing). Values are TOML: quote strings."""
import re
import sys
from pathlib import Path


def set_value(text, section, key, value):
    lines = text.split("\n")
    header = f"[{section}]"
    try:
        start = next(i for i, l in enumerate(lines) if l.strip() == header)
    except StopIteration:
        if lines and lines[-1] == "":
            lines.pop()
        return "\n".join(lines + ["", header, f"{key} = {value}", ""])
    end = next((i for i in range(start + 1, len(lines)) if lines[i].lstrip().startswith("[")),
               len(lines))
    pattern = re.compile(rf"^\s*{re.escape(key)}\s*=")
    for i in range(start + 1, end):
        if pattern.match(lines[i]):
            lines[i] = f"{key} = {value}"
            return "\n".join(lines)
    lines.insert(start + 1, f"{key} = {value}")
    return "\n".join(lines)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    # Bytes in and out, so the file keeps its line endings.
    text = Path(sys.argv[1]).read_bytes().decode("utf-8")
    crlf = "\r\n" in text
    text = text.replace("\r\n", "\n")
    settings = [("launcher", "show", "false"), ("patches", "skip_intro_logos", "true")]
    for arg in sys.argv[3:]:
        name, sep, value = arg.partition("=")
        section, dot, key = name.partition(".")
        if not sep or not dot or not section or not key:
            sys.exit(f"Not section.key=value: {arg}")
        settings.append((section, key, value))
    for section, key, value in settings:
        text = set_value(text, section, key, value)
    if crlf:
        text = text.replace("\n", "\r\n")
    Path(sys.argv[2]).write_bytes(text.encode("utf-8"))
    changed = ", ".join(f"{s}.{k}={v}" for s, k, v in settings[2:])
    print("Test settings: " + (changed or "the user's"))


if __name__ == "__main__":
    main()
