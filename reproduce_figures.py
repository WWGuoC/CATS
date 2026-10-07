"""Regenerate paper Figures 4--9 from the included experimental summaries."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent


def main():
    for name in ("generate_figures_editorial.py", "generate_figures_phase_e.py"):
        subprocess.run([sys.executable, str(ROOT / "experiments" / name)],
                       cwd=ROOT / "experiments", check=True)
    print(f"Figures 4--9 saved to {ROOT / 'figures'}")


if __name__ == "__main__":
    main()
