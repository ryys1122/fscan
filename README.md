# fscan

High-performance multi-threaded file counter and scanner.

## Build

```bash
g++ -std=c++17 -O3 -pthread -o fscan fscan.cpp
```

## Usage

```
fscan [options] <path> [path ...]
```

### Counting Options

| Option | Description |
|--------|-------------|
| `-a, --all` | Include hidden files/dirs (starting with `.`) |
| `-f, --follow` | Follow symbolic links |
| `-L, --max-depth N` | Limit recursion depth (default: unlimited) |
| `-T, --text` | Only count text files (skip binary) |
| `-j, --threads N` | Thread count (default: auto = CPU count) |
| `--since TIME` | Count files modified after TIME |
| `--until TIME` | Count files modified before TIME |
| `--exclude PAT` | Exclude names matching glob pattern (repeatable) |
| `--min-size SIZE` | Only count files >= SIZE (e.g. `1G`, `500M`) |
| `--max-size SIZE` | Only count files <= SIZE |

### Display Options

| Option | Description |
|--------|-------------|
| `-e, --extensions` | Show file count by extension |
| `-s, --size` | Show total file size |
| `-d, --depth` | Show directory depth statistics |
| `-t, --tree` | Tree-style output with per-dir counts |
| `-A, --align` | Align columns (truncates long names) |
| `-D, --dirs-only` | Only count directories |
| `-F, --find` | List individual files with sizes |
| `--unit UNIT` | Size unit: `auto` (default), `B`, `KB`, `MB`, `GB`, `TB` |
| `--csv` | CSV output |

### Time Format

`YYYY-MM-DD` or `YYYY-MM-DDTHH:MM:SS`

## Examples

```bash
# Count files in a directory
fscan src/

# Per-path with extensions and sizes
fscan -e -s *

# Tree view
fscan -t src/

# Exclude .o files
fscan --exclude '*.o' build/

# CSV output
fscan --csv -e -s /var/log

# Files modified since Jan 1, 2026
fscan --since 2026-01-01 /tmp

# Files >= 1GB
fscan --min-size 1G /var/log

# Find mode: list individual files with sizes
fscan -F --unit MB /home

# Text files only, max 3 levels deep
fscan -T -L 3 project/
```
