#!/usr/bin/env python3
"""Read and edit the per-key press counters of the keyboard over Bluetooth.

Talks to BlueZ over D-Bus and uses the connection the keyboard already has as
a HID device. It never connects or disconnects the device itself, so typing is
not interrupted. See README.md for setup and the protocol.
"""

import argparse
import asyncio
import json
import os
import re
import sys

from dbus_fast import BusType, Message, MessageType, Variant
from dbus_fast.aio import MessageBus

BLUEZ = "org.bluez"
OBJECT_MANAGER_IFACE = "org.freedesktop.DBus.ObjectManager"
DEVICE_IFACE = "org.bluez.Device1"
CHRC_IFACE = "org.bluez.GattCharacteristic1"

SERVICE_UUID = "a2b40001-9d84-4d17-986f-6b1daac0e668"
RX_UUID = "a2b40002-9d84-4d17-986f-6b1daac0e668"
TX_UUID = "a2b40003-9d84-4d17-986f-6b1daac0e668"

DEFAULT_NAME = "uniCorne"
PROTOCOL_VERSION = 1
# The firmware accepts one command per ATT write and has no long-write support.
MAX_COMMAND_LEN = 62
RESPONSE_TIMEOUT = 5.0
SERVICE_TIMEOUT = 10.0

# ATT errors the firmware answers with when it is not ready for a command yet.
ATT_ERR_NOT_SUBSCRIBED = 0xFD
ATT_ERR_BUSY = 0xFE
WRITE_RETRIES = 3
WRITE_RETRY_DELAY = 0.3

# Key positions as they sit on the keyboard, for `get --grid`.
GRID_ROWS = [
    (range(0, 6), range(6, 12)),
    (range(12, 18), range(18, 24)),
    (range(24, 30), range(30, 36)),
    (range(36, 39), range(39, 42)),
]

SERVICE_MISSING_HELP = """\
The keyboard is connected but does not offer the key stats service.

1. Make sure the left half runs firmware built with CONFIG_ZMK_KEYSTATS=y.
2. BlueZ may still have the old service list cached. Reconnect the keyboard:
       bluetoothctl disconnect {address}
   then press a key and try again.
3. If that does not help, clear the cache (the pairing is kept):
       sudo systemctl stop bluetooth
       sudo rm /var/lib/bluetooth/*/cache/{address}
       sudo systemctl start bluetooth
4. Last resort: `bluetoothctl remove {address}`, clear the profile on the
   keyboard (BT_CLR) and pair again."""


class KeystatsError(Exception):
    """An error to show to the user without a traceback."""


class AttError(KeystatsError):
    def __init__(self, code):
        super().__init__(f"The keyboard rejected the command (ATT error 0x{code:02x}).")
        self.code = code


async def bluez_call(bus, path, interface, member, signature="", body=None):
    reply = await bus.call(
        Message(
            destination=BLUEZ,
            path=path,
            interface=interface,
            member=member,
            signature=signature,
            body=body or [],
        )
    )
    if reply.message_type == MessageType.ERROR:
        text = reply.body[0] if reply.body else ""
        att_error = re.search(r"ATT error: 0x([0-9a-fA-F]{2})", str(text))
        if att_error:
            raise AttError(int(att_error.group(1), 16))
        raise KeystatsError(f"BlueZ: {reply.error_name}: {text}")
    return reply


async def managed_objects(bus):
    """Return {object path: {interface: {property: value}}} for everything BlueZ knows."""
    reply = await bluez_call(bus, "/", OBJECT_MANAGER_IFACE, "GetManagedObjects")
    return {
        path: {
            iface: {name: variant.value for name, variant in props.items()}
            for iface, props in ifaces.items()
        }
        for path, ifaces in reply.body[0].items()
    }


def find_device(objects, name, address):
    """Pick the paired keyboard by address or name. Returns (path, properties)."""
    paired = [
        (path, ifaces[DEVICE_IFACE])
        for path, ifaces in objects.items()
        if DEVICE_IFACE in ifaces and ifaces[DEVICE_IFACE].get("Paired")
    ]

    if address:
        matches = [d for d in paired if d[1].get("Address", "").lower() == address.lower()]
        wanted = f"with address {address}"
    else:
        matches = [
            d
            for d in paired
            if name.lower() in (d[1].get("Name", "").lower(), d[1].get("Alias", "").lower())
        ]
        wanted = f'named "{name}"'

    if not matches:
        known = ", ".join(sorted(f'{d[1].get("Alias", "?")} ({d[1].get("Address", "?")})' for d in paired))
        raise KeystatsError(f"No paired Bluetooth device {wanted}. Paired devices: {known or 'none'}.")

    connected = [d for d in matches if d[1].get("Connected")]
    if len(connected) > 1:
        addresses = ", ".join(d[1].get("Address", "?") for d in connected)
        raise KeystatsError(f"Several connected devices are {wanted}: {addresses}. Pick one with --address.")
    if not connected:
        raise KeystatsError("The keyboard is not connected. Press a key to wake it and try again.")

    return connected[0]


def find_characteristics(objects, device_path):
    """Return the D-Bus paths of the RX and TX characteristics, or None for a missing one."""
    found = {}
    for path, ifaces in objects.items():
        if CHRC_IFACE in ifaces and path.startswith(device_path + "/"):
            found[ifaces[CHRC_IFACE].get("UUID", "").lower()] = path
    return found.get(RX_UUID), found.get(TX_UUID)


class Link:
    """A command channel to the keyboard: one JSON command out, one JSON line back."""

    def __init__(self, bus, rx_path, tx_path, notify_fd):
        self._bus = bus
        self._rx_path = rx_path
        self._tx_path = tx_path
        self._fd = notify_fd
        self._buffer = bytearray()
        self._data_ready = asyncio.Event()
        self._closed = False
        asyncio.get_running_loop().add_reader(notify_fd, self._on_readable)

    @classmethod
    async def open(cls, name, address):
        bus = await MessageBus(bus_type=BusType.SYSTEM, negotiate_unix_fd=True).connect()
        try:
            objects = await managed_objects(bus)
            device_path, device = find_device(objects, name, address)

            # Right after a reconnect BlueZ may still be discovering services.
            deadline = asyncio.get_running_loop().time() + SERVICE_TIMEOUT
            while True:
                rx_path, tx_path = find_characteristics(objects, device_path)
                if rx_path and tx_path:
                    break
                resolved = objects.get(device_path, {}).get(DEVICE_IFACE, {}).get("ServicesResolved")
                if resolved or asyncio.get_running_loop().time() > deadline:
                    raise KeystatsError(SERVICE_MISSING_HELP.format(address=device.get("Address", "<MAC>")))
                await asyncio.sleep(0.5)
                objects = await managed_objects(bus)

            # BlueZ hands over a socket that yields one notification per read and
            # stops notifications by itself when the socket is closed.
            reply = await bluez_call(bus, tx_path, CHRC_IFACE, "AcquireNotify", "a{sv}", [{}])
            return cls(bus, rx_path, tx_path, reply.unix_fds[0])
        except BaseException:
            bus.disconnect()
            raise

    def close(self):
        if self._fd is not None:
            asyncio.get_running_loop().remove_reader(self._fd)
            os.close(self._fd)
            self._fd = None
        self._bus.disconnect()

    def _on_readable(self):
        try:
            data = os.read(self._fd, 512)
        except BlockingIOError:
            return
        except OSError:
            data = b""
        if data:
            self._buffer += data
        else:
            self._closed = True
            asyncio.get_running_loop().remove_reader(self._fd)
        self._data_ready.set()

    async def _read_line(self):
        while b"\n" not in self._buffer:
            if self._closed:
                raise KeystatsError("The keyboard disconnected while answering.")
            self._data_ready.clear()
            await self._data_ready.wait()
        line, _, rest = self._buffer.partition(b"\n")
        self._buffer = bytearray(rest)
        return bytes(line)

    async def _write(self, payload):
        for attempt in range(WRITE_RETRIES + 1):
            try:
                await bluez_call(
                    self._bus,
                    self._rx_path,
                    CHRC_IFACE,
                    "WriteValue",
                    "aya{sv}",
                    [payload, {"type": Variant("s", "request")}],
                )
                return
            except AttError as error:
                if error.code not in (ATT_ERR_BUSY, ATT_ERR_NOT_SUBSCRIBED) or attempt == WRITE_RETRIES:
                    raise
                await asyncio.sleep(WRITE_RETRY_DELAY)

    async def request(self, command):
        """Send one command and return the decoded reply."""
        payload = json.dumps(command, separators=(",", ":")).encode("ascii")
        if len(payload) > MAX_COMMAND_LEN:
            raise KeystatsError(f"Command is too long ({len(payload)} > {MAX_COMMAND_LEN} bytes).")

        # Drop leftovers of an earlier reply that timed out.
        self._buffer.clear()
        await self._write(payload)
        try:
            line = await asyncio.wait_for(self._read_line(), RESPONSE_TIMEOUT)
        except asyncio.TimeoutError:
            raise KeystatsError("The keyboard did not answer in time.") from None

        try:
            reply = json.loads(line)
        except ValueError:
            raise KeystatsError(f"The keyboard sent an invalid reply: {line!r}") from None
        if not isinstance(reply, dict):
            raise KeystatsError(f"The keyboard sent an unexpected reply: {line!r}")
        if reply.get("ok") is False:
            raise KeystatsError(f"The keyboard reported an error: {reply.get('err', 'unknown')}")
        return reply

    async def get(self):
        reply = await self.request({"cmd": "get"})
        keys = reply.get("keys")
        if reply.get("v") != PROTOCOL_VERSION or not isinstance(keys, list):
            raise KeystatsError(f"The keyboard sent an unsupported reply: {reply!r}")
        return reply


def load_counts(path):
    """Read counts from a file written by `get -o` (or a bare JSON list)."""
    try:
        with open(path, encoding="utf-8") as file:
            data = json.load(file)
    except (OSError, ValueError) as error:
        raise KeystatsError(f"Cannot read {path}: {error}") from None

    keys = data.get("keys") if isinstance(data, dict) else data
    valid = isinstance(keys, list) and all(
        isinstance(count, int) and not isinstance(count, bool) and 0 <= count <= 0xFFFFFFFF for count in keys
    )
    if not valid:
        raise KeystatsError(f'{path} must hold {{"keys": [...]}} with counts between 0 and 4294967295.')
    return keys


def format_grid(keys):
    if len(keys) != 42:
        raise KeystatsError(f"--grid needs the 42-key layout, but the keyboard reported {len(keys)} keys.")
    width = max(len(str(count)) for count in keys)
    half_width = 6 * (width + 1) - 1
    lines = []
    for left, right in GRID_ROWS:
        left_text = " ".join(str(keys[i]).rjust(width) for i in left)
        right_text = " ".join(str(keys[i]).rjust(width) for i in right)
        # Thumb rows are shorter and sit next to the gap between the halves.
        lines.append(f"{left_text.rjust(half_width)}   {right_text}".rstrip())
    return "\n".join(lines)


def confirm(question, assume_yes):
    if assume_yes:
        return True
    if not sys.stdin.isatty():
        raise KeystatsError("Refusing to continue without confirmation; pass --yes.")
    return input(f"{question} [y/N] ").strip().lower() in ("y", "yes")


async def run(args):
    if args.command == "restore":
        wanted = load_counts(args.file)
    if args.command == "set" and not 0 <= args.count <= 0xFFFFFFFF:
        raise KeystatsError("COUNT must be between 0 and 4294967295.")

    link = await Link.open(args.name, args.address)
    try:
        if args.command == "get":
            reply = await link.get()
            text = json.dumps(reply, separators=(",", ":"))
            if args.output:
                with open(args.output, "w", encoding="utf-8") as file:
                    file.write(text + "\n")
            print(format_grid(reply["keys"]) if args.grid else text)

        elif args.command == "set":
            await link.request({"cmd": "set", "pos": args.pos, "count": args.count})
            print(f"Key {args.pos} set to {args.count}.")

        elif args.command == "reset":
            if not confirm("Reset all key counts to zero?", args.yes):
                return 1
            await link.request({"cmd": "reset"})
            print("All key counts reset.")

        elif args.command == "save":
            await link.request({"cmd": "save"})
            print("Key counts saved to flash.")

        elif args.command == "restore":
            current = (await link.get())["keys"]
            if len(current) != len(wanted):
                raise KeystatsError(f"{args.file} has {len(wanted)} keys, but the keyboard has {len(current)}.")
            changes = [(pos, count) for pos, count in enumerate(wanted) if current[pos] != count]
            if not changes:
                print("Nothing to restore: the keyboard already has these counts.")
                return 0
            if not confirm(f"Overwrite {len(changes)} key counts on the keyboard?", args.yes):
                return 1
            for pos, count in changes:
                await link.request({"cmd": "set", "pos": pos, "count": count})
            await link.request({"cmd": "save"})
            print(f"Restored {len(changes)} key counts and saved them to flash.")
    finally:
        link.close()
    return 0


def parse_args(argv):
    parser = argparse.ArgumentParser(description="Read and edit the keyboard's per-key press counters over Bluetooth.")
    parser.add_argument("--name", default=DEFAULT_NAME, help=f"Bluetooth name of the keyboard (default: {DEFAULT_NAME})")
    parser.add_argument("--address", help="Bluetooth address of the keyboard, instead of --name")
    commands = parser.add_subparsers(dest="command", required=True)

    get = commands.add_parser("get", help="print the counts as JSON")
    get.add_argument("-o", "--output", metavar="FILE", help="also write the JSON to FILE")
    get.add_argument("--grid", action="store_true", help="print the counts laid out like the keyboard")

    set_ = commands.add_parser("set", help="set the count of one key")
    set_.add_argument("pos", type=int, metavar="POS", help="key position, starting at 0")
    set_.add_argument("count", type=int, metavar="COUNT", help="new count")

    reset = commands.add_parser("reset", help="set all counts to zero")
    reset.add_argument("-y", "--yes", action="store_true", help="do not ask for confirmation")

    commands.add_parser("save", help="write the counts to the keyboard's flash now")

    restore = commands.add_parser("restore", help="load counts from a file written by `get -o`")
    restore.add_argument("file", metavar="FILE")
    restore.add_argument("-y", "--yes", action="store_true", help="do not ask for confirmation")

    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        return asyncio.run(run(args))
    except KeystatsError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
