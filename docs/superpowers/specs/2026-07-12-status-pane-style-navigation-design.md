# Status Pane Style and Navigation Design

## Goal

Make the WinExInfo text look native beside Explorer's existing status-bar
content and keep it synchronized with the active tab's current path.

## Rendering

- Keep the uniquely identified `WinExInfo.StatusPane` host window.
- Render text through a child Windows `STATIC` control; do not custom-paint text.
- Apply the nearest Explorer status-bar font through `WM_GETFONT`, with
  `DEFAULT_GUI_FONT` only as a fallback.
- Use current system window text/background colors and refresh them on theme,
  settings, DPI, and font changes.
- Match the status bar's height, vertical centering, and eight-pixel horizontal
  inset. Keep single-line ellipsis and no focus activation.

## Navigation and State

- Every production observer snapshot identifies the exact active tab, its
  generation, and its filesystem path.
- The Host computes Git text for that path off the Explorer UI thread.
- The Host always sends a pane state for the active tab: non-empty text for a
  Git repository and an explicit empty state for non-Git, virtual, unavailable,
  or failed paths.
- The DLL applies a state only when top-level HWND/generation and active-tab
  HWND/generation still match its authoritative tab set.
- Non-empty state updates text and shows the pane. Empty state clears and hides
  it. A later layout refresh must preserve that visibility decision.
- Polling remains bounded at 250 ms. A stale Git result can never overwrite a
  newer navigation result.

## Error Handling

- Invalid or stale pane updates return a correlated failure and do not change
  the current UI.
- Git discovery/status errors are represented as an empty pane state, not a
  Host or Explorer failure.
- Rendering uses only system controls and messages; no heap allocation or Git
  work occurs in Explorer paint callbacks.

## Acceptance

- The text visually aligns with Explorer's own item-count status text in light,
  dark, and high-contrast modes.
- Navigating Git to Git replaces branch/count text.
- Navigating Git to non-Git hides the WinExInfo pane.
- Navigating back to Git shows the correct current repository state.
- Tab switches cannot display a prior tab's late result.
