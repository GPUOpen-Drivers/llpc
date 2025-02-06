#!/usr/bin/env python3
##
 #######################################################################################################################
 #
 #  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 #
 #  Permission is hereby granted, free of charge, to any person obtaining a copy
 #  of this software and associated documentation files (the "Software"), to
 #  deal in the Software without restriction, including without limitation the
 #  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 #  sell copies of the Software, and to permit persons to whom the Software is
 #  furnished to do so, subject to the following conditions:
 #
 #  The above copyright notice and this permission notice shall be included in all
 #  copies or substantial portions of the Software.
 #
 #  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 #  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 #  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 #  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 #  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 #  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 #  IN THE SOFTWARE.
 #
 #######################################################################################################################

# Map PCI device ids to a gfxip number

import glob
import sys

VENDOR_AMD = "1002"

def get_gfxip(device):
    # Get version of the GC (Graphics and Compute) block as exposed by the kernel
    with open(f"/sys/class/drm/card{device}/device/ip_discovery/die/0/GC/0/major") as f:
        major = int(f.read())
    with open(f"/sys/class/drm/card{device}/device/ip_discovery/die/0/GC/0/minor") as f:
        minor = int(f.read())
    return [major, minor]

def gfxip_to_str(ip):
    return ".".join([str(i) for i in ip])

def find_gfxips(device):
    """Find the gfxips of the given PCI device id"""
    ip = get_gfxip(device)
    [maj, min] = get_gfxip(device)

    gfxips = [ip]
    for maj_i in range(9, maj + 1):
        gfxips.append([maj_i])
    return ["gfx" + gfxip_to_str(ip)] + ["gfx" + gfxip_to_str(i) + "+" for i in gfxips]

def query_gfxips(device = None):
    """Find all gfxips of device or of the first AMD GPU on the system"""
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

    with open(f"/sys/class/drm/card{device}/device/device") as f:
        device_id = f.read().strip().replace("0x", "")

    gfxips = find_gfxips(device)
    print(f"Chosen device: gfx{gfxip_to_str(get_gfxip(device))} (card{device}, pci id 0x{device_id})")
    return gfxips

if __name__ == '__main__':
    print(query_gfxips(None if len(sys.argv) < 2 else sys.argv[1]))
