# Decision 000: repository layout

## Decision

Use a monorepo with explicit boundaries for firmware, model work, backend,
simulation, documentation, and presentation assets. Keep the existing ESP-IDF
project internally stable during the first migration.

## Reasoning

The project already contains a working firmware prototype and valuable test and
review material. Moving its internal files immediately would create path churn
without improving runtime. The first step therefore changes the repository
boundary, not firmware behavior.

## Consequences

- firmware/esp32 is the current source of truth for device behavior.
- Root-level documentation explains system boundaries and evidence standards.
- Generated data and local credentials are excluded from future commits.
- Backend and ML extraction can happen in focused, testable follow-up changes.
