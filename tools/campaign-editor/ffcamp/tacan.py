"""Airbase TACAN stations.

`stations.dat` in the campaign directory, read by `TacanList::TacanList`
(`src/sim/navaids/tacan.cpp`). Whitespace-separated, `#` and `;` comment out a
line:

    stationId channel band callsign range tactype ilsfreq

`stationId` is the objective's **campaign id**, which is what ties a station to
a place on the map. Only the first four fields are required; the loader fills
range 150, tactype 1 and ILS 111.1 when they are missing.
"""

import os


def _find(dirpath, filename):
    want = filename.lower()
    try:
        for name in os.listdir(dirpath):
            if name.lower() == want:
                return os.path.join(dirpath, name)
    except OSError:
        pass
    return None


def load(campaign_dir):
    """-> {campId: {channel, band, callsign, range, type, ils}}."""
    path = _find(campaign_dir, "stations.dat")
    out = {}
    if not path:
        return out
    try:
        fp = open(path, "r", encoding="latin-1")
    except OSError:
        return out

    with fp:
        for line in fp:
            line = line.strip()
            if not line or line[0] in "#;":
                continue
            parts = line.split()
            if len(parts) < 4:
                continue
            try:
                camp_id = int(parts[0])
                channel = int(parts[1])
            except ValueError:
                continue
            band = parts[2].upper()
            if band not in ("X", "Y"):
                continue

            def num(i, default, cast=int):
                try:
                    return cast(parts[i])
                except (IndexError, ValueError):
                    return default

            out[camp_id] = {
                "channel": channel,
                "band": band,
                "callsign": num(3, 0),
                "range": num(4, 150),
                "type": num(5, 1),
                "ils": num(6, 111.1, float),
                # What the game prints next to the station, e.g. "042X".
                "label": "%03d%s" % (channel, band),
            }
    return out
