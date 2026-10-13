# keystats-cli

Read, edit and reset the per-key press counters of the uniCorne keyboard over
Bluetooth.

The keyboard counts every key press per physical key (42 counters, both
halves, independent of the active layer), keeps the counts in flash, and
exposes them through a small Bluetooth service that speaks JSON. This folder
holds the host program and the description of that protocol.

## Requirements

- Linux with BlueZ (the tool talks to BlueZ over D-Bus).
- Python 3.9 or newer.
- The keyboard paired and connected over Bluetooth, with the left half running
  firmware built with `CONFIG_ZMK_KEYSTATS=y`.

The tool uses the connection the keyboard already has as a HID device. It never
connects or disconnects the keyboard, so typing is not interrupted.

## Setup

```sh
cd keystats-cli
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

## Usage

```sh
.venv/bin/python keystats.py get                 # counts as JSON
.venv/bin/python keystats.py get --grid          # counts laid out like the keyboard
.venv/bin/python keystats.py get -o backup.json  # also write the JSON to a file
.venv/bin/python keystats.py set 5 120           # set key 5 to 120 presses
.venv/bin/python keystats.py reset               # all counts to zero (asks first)
.venv/bin/python keystats.py save                # write counts to flash now
.venv/bin/python keystats.py restore backup.json # load counts from a file (asks first)
```

`reset` and `restore` accept `--yes` to skip the question.

The keyboard is found by its Bluetooth name, `uniCorne` by default. Use
`--name NAME` or `--address AA:BB:CC:DD:EE:FF` (before the command) to pick
another device.

`restore` reads a file written by `get -o` (or a bare JSON list of counts),
sends only the counts that differ, and then saves to flash.

### Key positions

`pos` is the index of the key in the keymap, starting at 0:

```
 0  1  2  3  4  5      6  7  8  9 10 11
12 13 14 15 16 17     18 19 20 21 22 23
24 25 26 27 28 29     30 31 32 33 34 35
         36 37 38     39 40 41
```

With the default layer that is:

```
ESC   Q  W  E  R  T      Y  U  I  O  P  BSPC
TAB   A  S  D  F  G      H  J  K  L  ;  '
CTRL  Z  X  C  V  B      N  M  ,  .  /  SHIFT
           GUI RAISE ENT     SPC LOWER RALT
```

## How the keyboard stores the counts

- A press is counted once when the key goes down. Holding a key does not add
  more, and the count is the same whatever layer is active.
- Counts live in RAM and are written to flash:
  - at most once every `CONFIG_ZMK_KEYSTATS_SAVE_INTERVAL` seconds while you
    type (default 600, i.e. 10 minutes),
  - whenever the keyboard goes idle or to sleep,
  - about 5 seconds after a `set` or `reset` from the host,
  - immediately on `save`.
- Nothing is written when no count changed.
- If the keyboard loses power abruptly while you are typing, up to one save
  interval of presses is lost.
- A counter stops at 4294967295 instead of wrapping around.
- Flashing the `settings_reset` firmware erases the counts together with all
  other settings. Use `get -o` first and `restore` afterwards to keep them.

Firmware options (set in `config/boards/shields/unicorne/unicorne_left.conf`):

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_ZMK_KEYSTATS` | `n` | Enable the counter. Central (left) half only. |
| `CONFIG_ZMK_KEYSTATS_SAVE_INTERVAL` | `600` | Max seconds between flash saves while typing (10 to 86400). |
| `CONFIG_ZMK_KEYSTATS_BLE` | `y` | Enable the Bluetooth service. |

## Protocol

Any BLE client can use the service; this tool is one implementation.

| | UUID | Properties |
|---|---|---|
| Service | `a2b40001-9d84-4d17-986f-6b1daac0e668` | |
| RX (commands) | `a2b40002-9d84-4d17-986f-6b1daac0e668` | write with response |
| TX (replies) | `a2b40003-9d84-4d17-986f-6b1daac0e668` | notify |

Both characteristics require an encrypted link, so only a paired host can use
them.

1. Enable notifications on TX.
2. Write one JSON object to RX. It must fit a single write: at most 62 bytes,
   so send it without extra whitespace.
3. The keyboard answers with exactly one JSON object followed by a newline
   (`\n`) on TX. Long replies are split across several notifications; join
   them until the newline arrives.
4. Send the next command only after the reply is complete.

| Command | Reply | Effect |
|---|---|---|
| `{"cmd":"get"}` | `{"v":1,"keys":[c0,c1,...]}` | Counts for every key position, in order. |
| `{"cmd":"set","pos":P,"count":N}` | `{"ok":true}` | Set key `P` to `N` (0 to 4294967295). |
| `{"cmd":"reset"}` | `{"ok":true}` | Set every count to zero. |
| `{"cmd":"save"}` | `{"ok":true}` | Write the counts to flash before replying. |

`v` is the protocol version. Unknown keys in a command are ignored. Strings
must not contain escape sequences, and numbers must be plain non-negative
integers.

Errors are reported as `{"ok":false,"err":"CODE"}`:

| Code | Meaning |
|---|---|
| `bad_json` | Not a flat JSON object of strings and non-negative integers. |
| `missing_field` | No `cmd`, or `set` without a numeric `pos` or `count`. |
| `unknown_cmd` | `cmd` is not one of the commands above. |
| `bad_pos` | `pos` is not a valid key position. |
| `bad_count` | `count` is larger than 4294967295. |
| `save_failed` | Writing to flash failed. |

Some problems are rejected at the Bluetooth level instead, as an ATT error on
the write, and produce no reply:

| ATT error | Meaning |
|---|---|
| `0x0d` | Command is empty or longer than 62 bytes. |
| `0xfd` | Notifications on TX are not enabled. |
| `0xfe` | The previous command is still being answered. |
| `0x03` | The writer is not a host (for example the other keyboard half). |

## Troubleshooting

**"The keyboard is not connected."** Press a key to wake it. The tool does not
start a connection by itself.

**"does not offer the key stats service"** after flashing new firmware: BlueZ
may still remember the old list of services.

1. Reconnect: `bluetoothctl disconnect <MAC>`, press a key, try again.
2. Clear the cache (the pairing is kept):
   ```sh
   sudo systemctl stop bluetooth
   sudo rm /var/lib/bluetooth/*/cache/<MAC>
   sudo systemctl start bluetooth
   ```
3. Last resort: `bluetoothctl remove <MAC>`, clear the Bluetooth profile on the
   keyboard (`BT_CLR` on the lower layer) and pair again.

**"The keyboard did not answer in time."** Run the command again. If it keeps
happening, reconnect the keyboard as in step 1 above.

**Commands feel slow.** The keyboard saves power by answering only every few
hundred milliseconds when idle, so each command can take up to about half a
second and a full `restore` several seconds.
