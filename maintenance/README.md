# Fork maintenance automation

Unattended upstream sync and issue/PR handling for this fork, run by the maintainer's machine through
systemd user timers (`hyprland-upstream-sync`, `hyprland-fork-triage`, `hyprland-fork-notify`), which point
at `~/.local/share/hyprland-maintenance` — a symlink to this directory on the maintainer's checkout.

- `run.py sync|triage|notifications [--check]` — the scheduler; `--check` only lists what would run.
- `instructions.txt` — the rules every agent run receives before its task.
- State, logs and per-run workspaces live outside the repository in `~/.local/state/hyprland-maintenance/`.
