#!/usr/bin/env python3
"""
cisco_sync.py - Autonomous Bluetooth Mobile & PBAP Contact Sync Daemon
Runs independently as part of the Cisco VoIP module.
Does NOT depend on main.py or cdpanel.
"""

import os
import sys
import time
import re
import json
import dbus
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

DIRECTORY_XML = "/srv/cisco/directory.xml"
CONTACTS_JSON = "/srv/cisco/contacts.json"
MOBILE_JSON = "/run/cisco_mobile.json"

def get_connected_device(bus):
    try:
        manager = dbus.Interface(bus.get_object("org.bluez", "/"), "org.freedesktop.DBus.ObjectManager")
        objects = manager.GetManagedObjects()
        for path, ifaces in objects.items():
            if "org.bluez.Device1" in ifaces:
                dev = ifaces["org.bluez.Device1"]
                if dev.get("Connected", False):
                    addr = str(dev.get("Address", ""))
                    name = str(dev.get("Alias") or dev.get("Name") or "Móvil")
                    bat = 0
                    if "org.bluez.Battery1" in ifaces:
                        try:
                            bat = int(ifaces["org.bluez.Battery1"].get("Percentage", 0))
                        except Exception:
                            pass
                    return {
                        "path": path,
                        "address": addr,
                        "name": name,
                        "battery": bat,
                        "connected": True
                    }
    except Exception as e:
        print(f"[Sync] Error querying BlueZ: {e}", flush=True)
    return None

def write_mobile_json(dev_info):
    try:
        data = {
            "name": dev_info["name"] if dev_info else "Desconectado",
            "mac": dev_info["address"] if dev_info else "-",
            "connected": bool(dev_info and dev_info.get("connected")),
            "battery": dev_info.get("battery", 0) if dev_info else 0,
            "bt_mode": "voip",
            "codec": "mSBC (Wideband Speech HD Voice 16kHz)" if dev_info else "-"
        }
        with open(MOBILE_JSON, "w") as f:
            json.dump(data, f)
    except Exception as e:
        print(f"[Sync] Error writing {MOBILE_JSON}: {e}", flush=True)

def sync_pbap_contacts(bus, mac_addr):
    vcf_path = "/tmp/phonebook.vcf"
    try:
        obex = bus.get_object("org.bluez.obex", "/org/bluez/obex")
        client = dbus.Interface(obex, "org.bluez.obex.Client1")
        session_path = client.CreateSession(mac_addr, {"Target": "pbap"})
        session_obj = bus.get_object("org.bluez.obex", session_path)
        pbap = dbus.Interface(session_obj, "org.bluez.obex.PhonebookAccess1")
        pbap.Select("int", "pb")

        loop = GLib.MainLoop()

        def on_props_changed(interface, changed, invalidated, path):
            if interface == "org.bluez.obex.Transfer1":
                status = changed.get("Status")
                if status in ("complete", "error"):
                    loop.quit()

        sub = bus.add_signal_receiver(
            on_props_changed,
            dbus_interface="org.freedesktop.DBus.Properties",
            signal_name="PropertiesChanged",
            path_keyword="path"
        )

        pbap.PullAll(vcf_path, {})
        GLib.timeout_add_seconds(10, loop.quit)
        loop.run()
        sub.remove()
        client.RemoveSession(session_path)

        if not os.path.exists(vcf_path):
            print("[Sync] PBAP file not received.", flush=True)
            return

        with open(vcf_path, "r", encoding="utf-8", errors="ignore") as f:
            raw_vcf = f.read()

        vcards = raw_vcf.split("BEGIN:VCARD")
        contacts = []
        for vc in vcards:
            if not vc.strip():
                continue
            fn_m = re.search(r"^FN(?:;[^:]*)?:(.*)$", vc, re.M)
            name = fn_m.group(1).strip() if fn_m else ""
            tels = re.findall(r"^TEL(?:;[^:]*)?:(.*)$", vc, re.M)
            for tel in tels:
                clean_tel = re.sub(r"[^0-9+*#]", "", tel.strip())
                if name and clean_tel:
                    contacts.append({"name": name, "telephone": clean_tel})

        contacts.sort(key=lambda x: x["name"].lower())

        os.makedirs(os.path.dirname(CONTACTS_JSON), exist_ok=True)
        with open(CONTACTS_JSON, "w", encoding="utf-8") as f:
            json.dump(contacts, f, ensure_ascii=False, indent=2)

        xml_lines = [
            "<CiscoIPPhoneDirectory>",
            "  <Title>Agenda Móvil</Title>",
            f"  <Prompt>{len(contacts)} Contactos</Prompt>"
        ]
        for c in contacts:
            esc_name = c["name"].replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            xml_lines.append("  <DirectoryEntry>")
            xml_lines.append(f"    <Name>{esc_name}</Name>")
            xml_lines.append(f"    <Telephone>{c['telephone']}</Telephone>")
            xml_lines.append("  </DirectoryEntry>")
        xml_lines.append("</CiscoIPPhoneDirectory>")

        with open(DIRECTORY_XML, "w", encoding="utf-8") as f:
            f.write("\n".join(xml_lines) + "\n")

        print(f"[Sync] Successfully synced {len(contacts)} contacts via PBAP to XML & JSON!", flush=True)

    except Exception as e:
        print(f"[Sync] PBAP sync error: {e}", flush=True)

def main():
    print("[Sync] Autonomous Cisco VoIP / Bluetooth Sync Daemon started.", flush=True)
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    last_mac = None
    synced_for_mac = None

    while True:
        try:
            dev = get_connected_device(bus)
            write_mobile_json(dev)

            if dev and dev["address"]:
                mac = dev["address"]
                if mac != synced_for_mac:
                    print(f"[Sync] New connected device {mac} ({dev['name']}). Syncing phonebook...", flush=True)
                    sync_pbap_contacts(bus, mac)
                    synced_for_mac = mac
            else:
                synced_for_mac = None

        except Exception as e:
            print(f"[Sync] Loop exception: {e}", flush=True)

        time.sleep(4)

if __name__ == "__main__":
    main()
