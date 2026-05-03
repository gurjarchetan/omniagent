# Contributing to OmniAgent

Thank you for considering a contribution! OmniAgent is a small, focused C11 daemon and we
aim to keep it that way — tiny binary, low RSS, zero unnecessary dependencies.

---

## Ways to contribute

- **Bug reports** — open an issue with the template, include kernel version and distro
- **Bug fixes** — fork, fix, open a PR with a clear description
- **New receivers** — e.g. eBPF-based metrics, FreeBSD kqueue, cgroup v2 stats
- **New exporters** — Prometheus Remote Write, InfluxDB line protocol, OpenSearch
- **Packaging** — `.deb`, `.rpm`, Alpine `apk`, Homebrew formula, NixOS module
- **Documentation** — typo fixes, better examples, translated docs
- **Tests** — unit tests for ring buffer, pool allocator, telemetry serialiser

---

## Before opening a PR

1. **Open an issue first** for non-trivial features — discuss the design before writing code.
2. **One concern per PR** — keep changes focused; large sweeping refactors are hard to review.
3. **Follow the code style** (see below).
4. **Confirm the checklist** at the bottom of the PR template.

---

## Development setup

```bash
# Install build dependencies (Ubuntu / Debian)
sudo apt-get install -y build-essential libzstd-dev

# Build with all sanitisers enabled
make debug

# Run with debug logging
sudo ./omniagent-debug -L localhost -Q 3100 -m 9100 -d

# Release build
make -j$(nproc)

# Clean everything
make clean
```

---

## Code style

| Rule | Details |
|------|---------|
| Standard | C11 (`-std=c11`), no compiler extensions in production paths |
| Indentation | 4 spaces, no tabs |
| Line length | 100 chars soft limit, 120 hard limit |
| Naming | `snake_case` for functions and variables; `SCREAMING_SNAKE` for macros |
| Headers | Include guards with `#ifndef OMNIAGENT_<NAME>_H` |
| Comments | `/* block style */` for file-level and function docs; `//` acceptable for single-line inline |
| No heap on hot path | Use `pool_alloc` / `pool_free` — no `malloc` / `free` in receivers or processor |
| Thread safety | Shared state must use `_Atomic` or a mutex — document which |
| Error handling | Always check return values; log with `LOG_ERROR` / `LOG_WARN` |

Run the release build before submitting — it uses `-Wall -Wextra -Wmissing-prototypes -Wshadow -Wformat=2`
and must produce **zero warnings**.

---

## PR checklist

- [ ] `make debug` — compiles with zero warnings, zero sanitiser errors
- [ ] `make` (release) — compiles with zero warnings
- [ ] No new `malloc` / `free` calls on the receiver or processor hot path
- [ ] New code has a brief `/* Purpose */` comment at the function level
- [ ] Tested on at least one of: Ubuntu 22.04, Debian 12, RHEL 9, Alpine 3.19
- [ ] `README.md` updated if a user-visible behaviour changed

---

## Commit message format

```
component: short imperative summary (≤72 chars)

Optional longer explanation of WHY (not what — the diff shows what).
Reference issues as "Fixes #123" or "Closes #123".
```

Examples:
```
procfs: add /proc/vmstat scraper for page fault metrics
inotify: respect OAGENT_LOG_INCLUDE filter on IN_CREATE events
Fixes #42
```

---

## Reporting security vulnerabilities

Please **do not** open a public issue for security vulnerabilities.
See [SECURITY.md](SECURITY.md) for the responsible disclosure process.

---

## License

By contributing you agree that your changes will be licensed under the [MIT License](LICENSE).
