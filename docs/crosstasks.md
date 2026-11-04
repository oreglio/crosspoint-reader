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
priority until the next sync.

The header shows how many tasks are still open and, at its right end, how many
are done (**N done**), followed by **↻** while changes made on the device are
waiting for the next sync (**3 done ↻**). Open tasks are sorted high
priority first, then normal, then low. High-priority titles are in bold until
the task is done; that is the only priority marker. Long titles wrap over two lines.

| Button | Action |
| --- | --- |
| Up / Down | Move the selection; Up from the first row reaches **N done** |
| Confirm | Tick the selected task, or reopen a finished one |
| Hold Confirm | Open the task's menu |
| Left | Add a task |
| Right | Open the selected task |
| Back | Leave Tasks (from the finished tasks: back to the open ones) |

- A task you tick moves straight into the finished ones, and the selection
  stays put, on the task that took its place.
- **Up** from the first row reaches **N done** in the header, highlighted like a
  selected row; **Confirm** (its hint reads **view**) or a tap on it shows the
  finished tasks instead of the open ones. The header then reads **Done (N)**
  with **M to do** on the right: the same gesture, or **Back**, comes back to
  the open tasks. Reopening the last finished task comes back by itself. With
  nothing done, there is no count and **Up** works as usual.
- **Settings > System > Tasks > Task text size** sets the list's text:
  **10** (the default), **12** or **14** (Lexend Deca, the notes' typeface). **Task spacing** puts more room
  between the rows: **Compact** (the default), **Comfortable** or **Spacious**.
- An empty list shows a **+ Add a task** row; **Confirm** on it adds a task.
- On touch devices, tapping a row does what **Confirm** does, and a long press
  opens the task's menu. Tapping the count in the header switches the list.

Holding **Confirm** on a task (about a second) opens its menu: **Tick** (or
**Reopen**), **View**, **Edit** (title, then priority), **Group**
(only when groups exist), **Select**,
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
| Confirm (or a tap) | Select or deselect the task; on the header count, switch the list |
| Right | Tick every selected task (finished ones stay as they are) |
| Left | Delete every selected task, after a confirmation |
| Back | Leave the selection without changing anything |

Switching between open and finished tasks deselects the tasks that leave the
screen, so an action never touches a row you can no longer see. **Delete done
tasks (N)** deletes every finished task at once, after a confirmation. Either way, the next sync applies the change on the
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

## Groups

Tasks can be sorted into groups such as *ADMINISTRATIF* or *MAISON*. Groups are
created, renamed, deleted and reordered on the server's web page only; the
device shows them, adds tasks into them and moves tasks between them. Without a
server, or while the server has no group, nothing changes in the list.

- Open tasks that belong to no group come first, at the top; then each group,
  in the order set on the web page, under its **title** (the group's name and
  how many of its tasks are open). Within a group, tasks keep the priority
  order described above. A group with no open task is hidden. Finished tasks
  show no group.
- A title is a row you can select with **Up** / **Down**. On a title the footer
  reads **Back | add | previous | next**:

| Button | On a group title |
| --- | --- |
| Up / Down | Move the selection |
| Confirm (or a tap on the title) | Add a task into this group |
| Left (**previous**) | Jump to the previous group's title (nothing at the first one; it does not wrap) |
| Right (**next**) | Jump to the next group's title (nothing at the last one) |
| Back | Leave Tasks |

- **Left** (**add**) on an ordinary task still adds an ungrouped task. Adding
  into a group asks for the title and the priority, as usual.
- To move a task, hold **Confirm** on it and choose **Group**: pick **No group**
  or one of the groups. On a task's page, **Left** edits the title, then the
  priority, then offers the same choice as a third step (only when groups
  exist). From the list, **Edit** stops after the priority. A moved task goes to
  the end of its new group until the next sync.
- Deleting a group on the web page does not delete its tasks: they go back to
  the ungrouped top of the list at the next sync.
- In selection mode, the cursor skips the titles.

## A Task's Page

The page shows the task's title, its note, and a pomodoro box.

| Button | Action |
| --- | --- |
| Up / Down | Turn the pages of a long note |
| Confirm | Tick the task, or reopen it once finished |
| Left | Edit the title, then the priority, then the group (when groups exist) |
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

### Groups and the Sync Protocol

Groups need a server that speaks sync **schema 2** (a groups-aware CrossDrop).
The device asks for it by sending `"schema": 2`, and the server answers with a
header `{"schema":2,...}` followed by the usual task lines plus group lines,
mixed in by change order:

- `{"group":"<id>","name":"...","position":N}` creates or updates a group;
- `{"group":"<id>","deleted":true}` removes one;
- a task line carries `"group":"<id>"` when the task is in a group (the key is
  absent otherwise).

The device sends its group changes as ops: `{"op":"group","id":"<task>","group":"<group id>"}`
(`"group":null` takes the task out of its group), and `{"op":"add",...}` may
carry a `group`. An op aimed at a group the server deleted in the meantime is
not an error: the sync completes and the task stays without a group.

A server that only knows schema 1 still works. It answers in schema 1, sends
no groups, and the device shows its tasks ungrouped. Until the server is
upgraded to a groups-aware version, a groups-aware device asks for a full list
at each sync: its cursor is cleared until it has done one complete schema 2
sync, which is also what brings the groups and the task assignments made while
it was syncing in schema 1. After that first complete schema 2 sync, syncs are
incremental again.

## Where the Data Lives

Everything is under `/.crosspoint/tasks/` on the SD card: the task index (which also holds the groups), the
queue of changes not yet synced, the sync position, notes (one text file per
task in `n/`), and the pairing code.
