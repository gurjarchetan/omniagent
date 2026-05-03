## Summary

<!-- What does this PR do? One paragraph is enough. -->

## Type of change

- [ ] Bug fix (non-breaking change that fixes an issue)
- [ ] New feature (non-breaking change that adds functionality)
- [ ] Breaking change (fix or feature that changes existing behaviour)
- [ ] Documentation update
- [ ] Refactor / code quality (no behaviour change)

## Related issue

Closes # <!-- issue number, or "N/A" -->

## Testing done

<!-- How did you verify this works? Which distro / kernel did you test on? -->

## Checklist

- [ ] `make debug` compiles with zero warnings and zero sanitiser errors
- [ ] `make` (release) compiles with zero warnings
- [ ] No new `malloc`/`free` calls on the receiver or processor hot path
- [ ] New functions have a `/* Purpose */` comment
- [ ] Tested on at least one distro (Ubuntu / Debian / RHEL / Alpine)
- [ ] `README.md` updated if user-visible behaviour changed
- [ ] `ARCHITECTURE.md` updated if thread/data-flow changed
