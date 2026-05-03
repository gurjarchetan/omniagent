# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| `main` (latest) | ✅ Active |
| Tagged releases | ✅ Security patches backported |
| Versions > 6 months old | ⚠️ Best effort |

---

## Reporting a vulnerability

**Please do not open a public GitHub issue for security vulnerabilities.**

If you find a security issue — including memory safety bugs (buffer overflows, use-after-free,
format string injection, integer overflows), privilege escalation via the procfs/Docker socket
path, or log injection — please disclose it privately:

1. Go to **Security → Report a vulnerability** on the GitHub repository page, or
2. Email the maintainer directly (address on the GitHub profile).

Please include:
- A description of the vulnerability and its potential impact
- Steps to reproduce (kernel version, distro, OmniAgent version / commit)
- Any proof-of-concept code (if applicable)

We will acknowledge your report within **72 hours** and aim to release a fix within **14 days**
for confirmed critical issues.

---

## Security design notes

OmniAgent is intended to run as `root` (or with `CAP_SYS_PTRACE` + `CAP_DAC_READ_SEARCH`)
because it reads `/proc/<pid>/` for all processes and talks to `/var/run/docker.sock`.

Known security considerations:

- **No untrusted input parsing** — all data sources are local kernel interfaces (`/proc`, Unix socket)
  or the local filesystem. OTLP/HTTP on `:4318` accepts JSON from local apps only (not exposed to the internet by default).
- **No config file parsing** — configuration is CLI flags and environment variables; no YAML/TOML parser attack surface.
- **Memory-safe by design** — all records come from a pre-allocated fixed slab pool; no unbounded allocation.
  Release builds are compiled with `-D_FORTIFY_SOURCE=2` and `-fstack-protector-strong`.
- **Debug builds validated with ASan + UBSan + LSan** (`make debug`) — run before each release.
- **SIGPIPE ignored** — prevents silent data loss on broken pipe; write errors are handled explicitly.

---

## Vulnerability disclosure history

_None yet._
