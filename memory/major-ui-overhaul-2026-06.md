---
name: major-ui-overhaul-2026-06
description: June 2026 large firmware refactor — what changed, where things live now
metadata:
  type: project
---

Major change landed in commit afa1068 (2026-06-02).

**Key decisions:**
- Settings screen moved to main/jpp_settings_screen.c/.h (not jpp_core/) to allow direct SSD1306 access without circular deps
- Footer parameter removed from jpp_sdk_set_frame() — existing apps (meetapp) updated
- App icons removed entirely — jpp_icons.h stubbed out
- "Files"/"fileserver" renamed to "FileDrop"/"filedrop" everywhere
- "about" builtin app removed
- JPPDOS_VERSION constant lives in main/jpp_settings_screen.h

**Why:** firmware UX overhaul by owner; 13-task batch change.

**How to apply:** When asked about settings, buzzer, wakelock, or UI structure, use the new file layout. set_frame() takes (lines, count) — no footer.
