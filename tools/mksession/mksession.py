#!/usr/bin/env python3
"""Write a DScheevos session file (session.bin) from saved server replies.

The DScheevos launcher writes this file after going online; this tool makes
the same file offline for tests (mock server replies, test sets).

  mksession.py --rom-path sd:/games/kirby.nds --hash <md5> --set set.json \
      [--login login.json] [--session start.json] [--softcore] [--spectator] -o session.bin

Layout: dscm_boot_t (see dscheevos/module/include/dscheevos_module.h), then
the login2, achievementsets and startsession replies, each followed by
1 to 4 NULs up to a multiple of 4 (DSCM_SESSION_PAD).
"""
import argparse
import calendar
import struct
import sys
import time

MAGIC = 0x53435344          # "DSCS"
BOOT_SIZE = 608             # sizeof(dscm_boot_t); checked by layout.c in CI
FLAGS_OFFSET = 462          # offsetof(dscm_boot_t, flags), as layout.c prints it
SESSION_MAX = 0x60000       # DSCM_SESSION_MAX
BOOT_SPECTATOR = 0x01       # DSCM_BOOT_SPECTATOR
BOOT_UTC_KNOWN = 0x02         # DSCM_BOOT_UTC_KNOWN

DEFAULT_LOGIN = b'{"Success":true,"User":"tester","Token":"tok","Score":0,"SoftcoreScore":0,"Messages":0}'
DEFAULT_SESSION = b'{"Success":true,"Unlocks":[],"HardcoreUnlocks":[],"ServerNow":%d}'


def fixed(text, size):
    data = text.encode()
    if len(data) >= size:
        sys.exit(f"value too long for a {size}-byte field: {text!r}")
    return data + b"\0" * (size - len(data))


def build(args):
    replies = [
        open(args.login, "rb").read() if args.login else DEFAULT_LOGIN,
        open(args.set, "rb").read(),
        open(args.session, "rb").read() if args.session else DEFAULT_SESSION % int(time.time()),
    ]
    offsets, lengths, body = [], [], b""
    offset = BOOT_SIZE
    for reply in replies:
        offsets.append(offset)
        lengths.append(len(reply))
        pad = 4 - len(reply) % 4      # DSCM_SESSION_PAD: at least one NUL
        body += reply + b"\0" * pad
        offset += len(reply) + pad
    file_size = offset
    if file_size > SESSION_MAX:
        sys.exit(f"session file too big: {file_size} > {SESSION_MAX}")

    head = struct.pack("<III", MAGIC, BOOT_SIZE, file_size)
    head += fixed(args.rom_path, 256)
    head += fixed(args.host, 64) + fixed(args.user, 32) + fixed(args.token, 64)
    head += fixed(args.hash.lower(), 33)
    head += struct.pack("<B", 0 if args.softcore else 1)    # hardcore
    assert len(head) == FLAGS_OFFSET, len(head)
    # flags, utc_offset_q: the console clock is the host's local time here, so
    # its offset from UTC is known, as the launcher measures it online
    offset_q = round(time.localtime().tm_gmtoff / 900)
    utc = not args.no_utc and -56 <= offset_q <= 56
    head += struct.pack("<Bb", (BOOT_SPECTATOR if args.spectator else 0) | (BOOT_UTC_KNOWN if utc else 0),
                        offset_q if utc else 0)
    # the console clock keeps local time and has no time zone: nds-bootstrap
    # compares this against it and refuses an old session
    head += struct.pack("<II", 0, int(args.unix_time or calendar.timegm(time.localtime())))
    head += struct.pack("<3I", *offsets) + struct.pack("<3I", *lengths)
    # network during play (mac, valid, ip, netmask, gateway, dns) and extra CA: none offline
    head += bytes(6) + struct.pack("<BB", 1 if args.net else 0, 0) + struct.pack("<4I", 0, 0, 0, 0) + struct.pack("<II", 0, 0)
    # access point (ssid, ssid_len, bssid, channel, security, ciphers); nds-bootstrap fills the PMK in RAM
    ssid = args.ap_ssid.encode()
    head += fixed(args.ap_ssid, 33)[:32] + struct.pack("<B", len(ssid)) + bytes(6)
    head += struct.pack("<BBBB", 6, args.ap_security, 4, 4) + bytes(1 + 32)
    # phone_ip (Cheevotron): no phone here
    head += struct.pack("<I", 0)
    assert len(head) == BOOT_SIZE, len(head)
    return head + body


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom-path", required=True, help="NDS_PATH as nds-bootstrap.ini has it")
    ap.add_argument("--hash", required=True, help="RetroAchievements hash of the ROM")
    ap.add_argument("--set", required=True, help="achievementsets reply (JSON)")
    ap.add_argument("--login", help="login2 reply (JSON)")
    ap.add_argument("--session", help="startsession reply (JSON)")
    ap.add_argument("--host", default="http://10.64.0.1:8080")
    ap.add_argument("--user", default="tester")
    ap.add_argument("--token", default="tok")
    ap.add_argument("--softcore", action="store_true")
    ap.add_argument("--spectator", action="store_true", help="rc_client spectator mode: evaluate, send nothing")
    ap.add_argument("--unix-time", type=int)
    ap.add_argument("--no-utc", action="store_true", help="leave the console clock's UTC offset unknown")
    ap.add_argument("--net", action="store_true", help="mark the network as usable during play (tests)")
    ap.add_argument("--ap-ssid", default="", help="access point the module may rejoin (tests)")
    ap.add_argument("--ap-security", type=int, default=0, help="0 open, 3 WPA2")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()
    data = build(args)
    with open(args.output, "wb") as f:
        f.write(data)
    print(f"wrote {args.output}: {len(data)} bytes")


if __name__ == "__main__":
    main()
