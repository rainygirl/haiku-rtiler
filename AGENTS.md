# R Tiler — development notes

## Moving other applications' windows

Through Haiku's public scripting protocol only. `BWindow` exposes `Frame`,
`Look`, `Hidden`, `Minimize` and `Workspaces` as scriptable properties, so no
private headers are involved and nothing breaks between releases. It is the
same mechanism `hey` uses.

`Look` is what filters the window list: only `B_TITLED_WINDOW_LOOK` and
`B_DOCUMENT_WINDOW_LOOK` are moved, which drops panels, menus, Tracker's
Desktop window and anything borderless without needing a name list.

## The tray item runs inside Deskbar

A fault here takes the Deskbar with it. Every message out has a timeout
(200 ms send, 400 ms reply), and the scan runs on its own thread, so an
application that is busy or wedged cannot freeze the tray.

## Install order matters

The tray item must be removed **before** the binary is replaced. Deskbar
keeps this executable loaded as an add-on and calls the replicant's
destructor through a vtable inside that image, so overwriting the file first
makes the removal jump into an image that no longer matches — which takes the
whole Deskbar down with a null instruction pointer:

```
BShelf::DeleteReplicant  <- TReplicantTray::RemoveIcon
current PC (nil)
```

`install.sh` removes, waits, then copies.

## Margins are applied to the content rect

The scripting protocol only exposes `Frame`, which is the content rectangle;
the decorator draws its border a few pixels outside it on every side and the
tab above it. So the visible gap between two windows is narrower than
`2 * kMargin` suggests, and `kTabHeight` has to be added to the top or the
tab of every window in the first row lands off screen. Neither the border
width nor the tab height has a public API — both are measured against the
default decorator at the default font size.

## Icon

Drawn in code rather than shipped as an asset: three panes butted together,
with the middle one filled. At 16 pixels three outlines alone blur into a
single box.
