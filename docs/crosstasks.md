---
title: Tasks
nav_order: 7.8
---

# Tasks

Tasks is a to-do list kept on the SD card. It works entirely offline: adding,
ticking, editing and reading tasks never needs a network. If you want the same
list on a computer or phone, the device can sync with a self-hosted
[CrossDrop](https://github.com/oreglio/crossdrop) server, and it does so only
when you ask.

## Opening Tasks

**Tasks** is on the home menu and is shown by default. To hide it, turn off
**Enable Tasks** in the **Tasks** card of the web portal's settings page.

## The List

Within a priority, tasks follow the order set on the server's web page, where
each open task can be dragged by its handle (never into another priority). A
task added or moved to another priority on the device goes to the end of its
group until the next sync.

The header shows how many tasks are still open, with **↻** at its right end while changes
made on the device are waiting for the next sync. Open tasks are sorted high
priority first, then normal, then low. High-priority titles are in bold until
the task is done; that is the only priority marker. Long titles wrap over two lines.

| Button | Action |
| --- | --- |
| Up / Down | Move the selection |
| Confirm | Tick the selected task, or reopen a finished one |
| Hold Confirm | Open the task's menu |
| Left | Add a task |
| Right | Open the selected task |
| Back | Leave Tasks |

- A task you tick moves straight into the finished ones, and the selection
  stays put, on the task that took its place.
- Finished tasks gather in an **N done** row below the open ones. Select it and
  press **Confirm** to expand or collapse it; the button hint says which.
- **Settings > System > Tasks > Task text size** sets the list's text:
  **10** (the default), **12** or **14** (Lexend Deca, the notes' typeface). **Task spacing** puts more room
  between the rows: **Compact** (the default), **Comfortable** or **Spacious**.
- An empty list shows a **+ Add a task** row; **Confirm** on it adds a task.
- On touch devices, tapping a row does what **Confirm** does, and a long press
  opens the task's menu.

Holding **Confirm** on a task (about a second) opens its menu: **Tick** (or
**Reopen**), **View**, **Edit** (title, then priority), **Select**,
**Delete done tasks (N)** (only when some are done), **Keep screen on**,
**Task text size** (the same setting as in Settings, applied on the spot),
**Delete** and, last, **Sync tasks**. **Keep screen on** stops the device from
going to sleep while you stay in Tasks (the list and a task's page); leaving
Tasks turns it off again. Deleting asks for confirmation first; the task disappears at once
and the next sync deletes it on the server too. **Sync tasks** from this menu
comes back to the list when the sync screen closes.

**Select** turns the list into a selection: round bullets become square boxes,
the title reads **Selection (N)**, and the task you held starts out selected.

| Button | In selection |
| --- | --- |
| Confirm (or a tap) | Select or deselect the task; on **N done**, expand or collapse |
| Right | Tick every selected task (finished ones stay as they are) |
| Left | Delete every selected task, after a confirmation |
| Back | Leave the selection without changing anything |

Collapsing **N done** deselects the finished tasks, so an action never touches a
row you can no longer see. **Delete done tasks (N)** deletes every finished task
at once, after a confirmation. Either way, the next sync applies the change on the
server too. The server's web page has the same tools above its list: **Select**
(with **All**, **Tick**, **Delete**) and **Clear done**, each deletion confirmed first.

Adding a task opens the keyboard for the title, then asks for a priority (High,
Normal or Low). Cancelling either step creates nothing. The list holds up to
120 tasks; once it is full, **Left** shows *Task list is full* instead of
opening the keyboard.

If a change cannot be written to the card, a message *Could not save* appears
for a moment over the list. If the device is too short of memory to add a
task, **Left** shows *Not enough memory* instead of opening the keyboard;
restart the device and try again.

## A Task's Page

The page shows the task's title, its note, and a pomodoro box.

| Button | Action |
| --- | --- |
| Up / Down | Turn the pages of a long note |
| Confirm | Tick the task, or reopen it once finished |
| Left | Edit the title, then the priority |
| Right | Open the pomodoro timer for this task |
| Back | Return to the list |

Notes are written on the server's web page; the device shows them but cannot
edit them. The pomodoro timer is the one behind **Countdown > Pomodoro** on
the home menu, with the task's title shown above the ring.

## Setting Up a Server

Syncing needs a CrossDrop server of your own, reachable from the device over
HTTPS, with a certificate that chains to ISRG Root X1, as Let's Encrypt
certificates do; the device trusts no other root for this connection. See the [CrossDrop repository](https://github.com/oreglio/crossdrop) for
how to run it.

Enter the server's address (for example `https://tasks.example.com`) in
**Server URL**, in the **Tasks** card of the web portal's settings page. It
cannot be typed on the device.

## Pairing

1. On the device, open **Settings > System > Tasks > Pair with server**. It shows a QR
   code and the same 26-character code, split into groups of four.
2. On the server, open the pairing page from the task list and scan the QR code
   or type the code.

The code is stored on the SD card in obfuscated form, not in the settings file,
and it stays the same each time you open the screen. **Right** (**change**)
makes a new one after a confirmation. Doing so unpairs the device: syncing
stops until you pair the new code on the server.

## Syncing

Open **Settings > System > Tasks > Sync tasks**, or **Sync tasks** from a task's menu. The device restarts into a small
network mode, joins Wi-Fi, sends the changes made on the device, then applies
the server's. It ends on a summary: tasks received, sent and in total, plus
any change the server refused and why (for example, a task that was deleted on
the server).

If a sync fails for any reason, your pending changes are never lost: they stay
queued, and the next sync resumes where the last one stopped, without sending
the already accepted changes twice. An interrupted sync may already have
applied part of the server's changes; the next sync completes them. A very
large backlog may end on *Sync incomplete*; run the sync again to finish it.

The failure screen says what went wrong: Wi-Fi not connected, no server URL
set, device not paired (*Pair again from Settings*), server unreachable, a
server error, not enough memory, or an incomplete response.

## Where the Data Lives

Everything is under `/.crosspoint/tasks/` on the SD card: the task index, the
queue of changes not yet synced, the sync position, notes (one text file per
task in `n/`), and the pairing code.
