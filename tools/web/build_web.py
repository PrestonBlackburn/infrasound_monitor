# In charge of rendering any HTML Jinja

# Renders Jinja pages and gzips them plus static assets, flattened by filename.
# Usage (CMake): build_web.py <templates_dir> <static_dir> <output_dir>

import gzip
import sys
from pathlib import Path
from jinja2 import Environment, FileSystemLoader

ROOT = Path(__file__).resolve().parents[2]

def get_jinja_env(template_dir:str) -> Environment:
    env = Environment(loader=FileSystemLoader(template_dir))
    return env

def render_pages(template_dir: Path, static_dir: Path, output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    env = Environment(loader=FileSystemLoader(template_dir))
    written: set[str] = set()

    def emit(name: str, data: bytes) -> None:
        if name in written:
            sys.exit(f"build_web: duplicate asset name '{name}' (names are flattened)")
        written.add(name)
        # mtime=0 keeps output deterministic, so unchanged inputs give identical files
        (output_dir / f"{name}.gz").write_bytes(gzip.compress(data, 9, mtime=0))

    # Pages: only templates/pages/*.html (index.html is just a base template)
    for tpl in sorted((template_dir / "pages").glob("*.html")):
        html = env.get_template(f"pages/{tpl.name}").render()
        emit(tpl.name, html.encode("utf-8"))

    # Static assets: recurse into css/, js/, img/, flattened to bare filenames
    for f in sorted(static_dir.rglob("*")):
        if f.is_file():
            emit(f.name, f.read_bytes())

    # Remove stale outputs from files that no longer exist
    for old in output_dir.glob("*.gz"):
        if old.name[:-3] not in written:
            old.unlink()


if __name__ == "__main__":
    if len(sys.argv) == 4:
        tpl_dir, static_dir, out_dir = map(Path, sys.argv[1:4])
    else:  # manual run, no arguments
        tpl_dir = ROOT / "templates"
        static_dir = ROOT / "static"
        out_dir = ROOT / "build" / "web_out"
    render_pages(tpl_dir, static_dir, out_dir)