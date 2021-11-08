#!/usr/bin/env python3
# Map PCI device ids to a gfxip number
# The list of PCI ids is taken from https://pci-ids.ucw.cz/read/PC/1002
import glob
import sys

VENDOR_AMD = "1002"

device_to_gfxip = {
    "13e9": "10.1",
    "1478": "10.1",
    "15d8": "9",
    "15dd": "9",
    "15e7": "9",
    "1607": "10.3",
    "1636": "9",
    "1638": "9",
    "163f": "10.3",
    "164c": "9",
    "164d": "10.3",
    "1681": "10.3",

    "66a0": "9",
    "66a1": "9",
    "66a2": "9",
    "66a3": "9",
    "66a7": "9",
    "66af": "9",

    "67c0": "8",
    "67c2": "8",
    "67c4": "8",
    "67c7": "8",
    "67ca": "8",
    "67cc": "8",
    "67cf": "8",
    "67d0": "8",
    "67d4": "8",
    "67d7": "8",
    "67df": "8",
    "67e0": "8",
    "67e1": "8",
    "67e3": "8",
    "67e8": "8",
    "67e9": "8",
    "67eb": "8",
    "67ef": "8",
    "67ff": "8",

    "6860": "9",
    "6861": "9",
    "6862": "9",
    "6863": "9",
    "6864": "9",
    "6867": "9",
    "6868": "9",
    "6869": "9",
    "686a": "9",
    "686b": "9",
    "686c": "9",
    "686d": "9",
    "686e": "9",
    "687f": "9",

    "694c": "8",
    "694e": "8",
    "694f": "8",
    "6980": "8",
    "6986": "8",

    "69a0": "9",
    "69a1": "9",
    "69a2": "9",
    "69a3": "9",
    "69af": "9",

    "6fdf": "8",

    "7310": "10.1",
    "7312": "10.1",
    "7314": "10.1",
    "731f": "10.1",
    "7340": "10.1",
    "7341": "10.1",
    "7347": "10.1",
    "734f": "10.1",
    "7360": "10.1",
    "7362": "10.1",

    "73a2": "10.3",
    "73a3": "10.3",
    "73a4": "10.3",
    "73ab": "10.3",
    "73af": "10.3",
    "73bf": "10.3",
    "73c3": "10.3",
    "73c4": "10.3",
    "73df": "10.3",
    "73e0": "10.3",
    "73e1": "10.3",
    "73e3": "10.3",
    "73e4": "10.3",
    "73ff": "10.3",

    "7408": "9",
    "740c": "9",
    "740f": "9",
}

def parse_gfxip(s):
    """Returns [major, minor, patch]"""
    arr = [int(i) for i in s.split(".")]
    while len(arr) < 3:
        arr.append(0)
    return arr

def gfxip_to_str(ip):
    return ".".join([str(i) for i in ip])

def find_gfxips(device_id):
    """Find the gxfips of the given PCI device id"""
    ip = parse_gfxip(device_to_gfxip[device_id])
    [maj, min, pat] = ip

    gfxips = [[maj, min], [maj, min, pat]]
    for maj_i in range(9, maj + 1):
        gfxips.append([maj_i])
    return ["gfx" + gfxip_to_str(ip)] + ["gfx" + gfxip_to_str(i) + "+" for i in gfxips]

def query_gfxips(device = None):
    """Find all gxfips of device or of the first AMD GPU on the system"""
    if device is None:
        amd_cards = []
        for card in glob.glob("/sys/class/drm/card*"):
            try:
                with open(card + "/device/vendor") as f:
                    vendor_id = f.read().strip()
                    if vendor_id == f"0x{VENDOR_AMD}":
                        amd_cards.append(int(card[19:]))
            except:
                # Ignore
                pass
        if len(amd_cards) == 0:
            raise Exception(f"Found no AMD card on the system in /sys/class/drm/card*")
        elif len(amd_cards) > 1:
            raise Exception(f"Found multiple AMD cards {amd_cards} on the system, please specify device explicitly")
        device = amd_cards[0]

    with open(f"/sys/class/drm/card{device}/device/vendor") as f:
        vendor_id = f.read().strip()
        if vendor_id != f"0x{VENDOR_AMD}":
            raise Exception(f"Vendor {vendor_id} is not AMD (0x{VENDOR_AMD})")

    with open("/sys/class/drm/card0/device/device") as f:
        device_id = f.read().strip().replace("0x", "")
    return find_gfxips(device_id)

if __name__ == '__main__':
    print(query_gfxips(None if len(sys.argv) < 2 else sys.argv[1]))
