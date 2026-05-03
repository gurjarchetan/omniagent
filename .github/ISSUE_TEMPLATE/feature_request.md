---
name: Feature request
about: Suggest a new receiver, exporter, metric, or capability
title: "[FEATURE] "
labels: enhancement
assignees: ''
---

## Summary

A one-sentence description of the feature.

## Motivation

Why is this useful? What problem does it solve? Who would benefit?

## Proposed design

How should it work? New CLI flag? Env var? New receiver thread?
Keep in mind OmniAgent's core constraints:
- No heap allocations on the hot path
- No new mandatory runtime dependencies
- Binary size and RSS should stay minimal

## Alternatives considered

What other approaches did you consider and why did you rule them out?

## Additional context

Links, references, similar implementations in other projects, etc.
