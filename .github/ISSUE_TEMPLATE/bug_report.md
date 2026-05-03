---
name: Bug report
about: Something is broken or behaving unexpectedly
title: "[BUG] "
labels: bug
assignees: ''
---

## Describe the bug

A clear and concise description of what is wrong.

## Environment

| Field | Value |
|-------|-------|
| OmniAgent version / commit | |
| Linux distro + version | |
| Kernel version (`uname -r`) | |
| Architecture (`uname -m`) | |
| Running as | root / non-root / Docker container |
| Docker version (if applicable) | |

## Steps to reproduce

1. Start OmniAgent with: `sudo ./omniagent ...`
2. ...
3. See error

## Expected behavior

What you expected to happen.

## Actual behavior

What actually happens. Include any error output.

## Logs

<details>
<summary>OmniAgent output (run with <code>-d</code> for debug logs)</summary>

```
paste logs here
```

</details>

## Metrics endpoint output (if relevant)

```
curl http://localhost:9100/metrics | grep <relevant_prefix>
```

## Additional context

Any other context, screenshots, or information.
