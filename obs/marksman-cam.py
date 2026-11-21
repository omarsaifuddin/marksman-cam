"""Marksman Cam controls for OBS (Tools > Scripts > + > marksman-cam.py).

Talks to a running marksman-cam over its control port on 127.0.0.1: 47811 Canon, 47812 Sony, 47810 any
make. Picture controls (brightness, zoom, mirror, ...) are always there; the camera's own settings
(ISO, shutter, aperture, white balance, focus, ...) appear while an app is showing that camera, because
marksman-cam only wakes the camera then. Press Refresh after the camera has come up.
"""
import json
import socket

import obspython as obs

PORTS = [("Canon", 47811), ("Sony", 47812), ("Any make", 47810)]
FOCUS = [(-3, "<<< nearer"), (-2, "<< nearer"), (-1, "< nearer"), (1, "farther >"), (2, "farther >>"), (3, "farther >>>")]

settings = None
port = PORTS[0][1]
shown = []      # property names added for the current control list
current = {}    # control name -> value as marksman-cam last reported it
status = ""


def request(line):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as s:
        s.sendall((line + "\n").encode())
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                return data.decode("utf-8", "replace")
            data += chunk


def fetch():
    global status
    try:
        reply = json.loads(request("list"))
    except (OSError, ValueError) as e:
        status = "marksman-cam isn't answering on 127.0.0.1:%d (%s)" % (port, e)
        return []
    status = ("Camera: %s" % reply["make"]) if reply.get("connected") else \
        "Camera asleep: show the Marksman Cam source in OBS, wait for the picture, then press Refresh"
    return reply.get("controls", [])


def send(name, value):
    try:
        reply = request("set %s %d" % (name, int(value))).strip()
    except OSError as e:
        reply = "error: %s" % e
    if reply != "ok":
        obs.script_log(obs.LOG_WARNING, "marksman-cam: set %s %s: %s" % (name, value, reply))
    else:
        current[name] = int(value)


def key(name):
    return "mc_" + name


def on_change(props, prop, data):
    name = obs.obs_property_name(prop)[3:]
    kind = obs.obs_property_get_type(prop)
    if kind == obs.OBS_PROPERTY_BOOL:
        value = int(obs.obs_data_get_bool(data, key(name)))
    else:
        value = obs.obs_data_get_int(data, key(name))
    if current.get(name) != value:  # also fires when the UI is (re)built; only real changes go out
        send(name, value)
    return False


def on_button(props, prop):
    name = obs.obs_property_name(prop)[3:]
    if name.startswith("focus_"):
        send("focus", int(name[6:]))
    else:
        send(name, 1)
    return False


def build(props):
    global shown
    for name in shown:
        obs.obs_properties_remove_by_name(props, name)
    shown = []
    controls = fetch()

    def add(p):
        shown.append(obs.obs_property_name(p))
        return p

    add(obs.obs_properties_add_text(props, "mc_status", status, obs.OBS_TEXT_INFO))
    current.clear()
    for c in controls:
        name, label, kind = c["name"], c["label"], c["type"]
        if kind == "button":
            add(obs.obs_properties_add_button(props, key(name), label, on_button))
            continue
        if kind == "step":
            for step, text in FOCUS:
                add(obs.obs_properties_add_button(props, key("focus_%d" % step), "Focus " + text, on_button))
            continue
        if kind == "info":
            add(obs.obs_properties_add_text(props, key(name), "%s: %s" % (label, c.get("text", "")), obs.OBS_TEXT_INFO))
            continue
        value = int(c.get("value", 0))
        current[name] = value
        if kind == "range":
            p = obs.obs_properties_add_int_slider(props, key(name), label, int(c["min"]), int(c["max"]), int(c.get("step", 1)))
            obs.obs_data_set_int(settings, key(name), value)
        elif kind == "bool":
            p = obs.obs_properties_add_bool(props, key(name), label)
            obs.obs_data_set_bool(settings, key(name), bool(value))
        elif kind == "menu":
            p = obs.obs_properties_add_list(props, key(name), label, obs.OBS_COMBO_TYPE_LIST, obs.OBS_COMBO_FORMAT_INT)
            for opt_value, opt_label in c.get("options", []):
                obs.obs_property_list_add_int(p, opt_label, int(opt_value))
            obs.obs_data_set_int(settings, key(name), value)
        else:
            continue
        add(p)
        if c.get("ro"):
            obs.obs_property_set_enabled(p, False)
        obs.obs_property_set_modified_callback(p, on_change)


def on_refresh(props, prop):
    build(props)
    return True


def on_port(props, prop, data):
    global port
    new = obs.obs_data_get_int(data, "port") or PORTS[0][1]
    if new != port:
        port = new
        build(props)
        return True
    return False


def script_description():
    return ("<b>Marksman Cam</b><br>Camera settings and picture controls for marksman-cam. "
            "Camera settings show while OBS is displaying the camera; press Refresh once the picture is up.")


def script_defaults(data):
    obs.obs_data_set_default_int(data, "port", PORTS[0][1])


def script_load(data):
    global settings, port
    settings = data
    port = obs.obs_data_get_int(data, "port") or PORTS[0][1]


def script_properties():
    props = obs.obs_properties_create()
    p = obs.obs_properties_add_list(props, "port", "Camera", obs.OBS_COMBO_TYPE_LIST, obs.OBS_COMBO_FORMAT_INT)
    for label, number in PORTS:
        obs.obs_property_list_add_int(p, label, number)
    obs.obs_property_set_modified_callback(p, on_port)
    obs.obs_properties_add_button(props, "refresh", "Refresh", on_refresh)
    global shown
    shown = []  # a fresh properties object: nothing to remove
    build(props)
    return props
