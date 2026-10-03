#!/usr/bin/env python3
"""Tier 0 demo: generate a Preset Connector manifest.json for Odin 2's factory presets with no plugin code involved."""
import os, json
root = os.path.join(os.path.dirname(__file__), '..', 'assets', 'Soundbanks', 'Factory Presets')
presets, cats = [], {}
for cat in sorted(os.listdir(root)):
    d = os.path.join(root, cat)
    if not os.path.isdir(d): continue
    for f in sorted(x for x in os.listdir(d) if x.endswith('.odin')):
        pid = f"factory/{cat}/{f[:-5]}"
        presets.append({"id": pid, "name": f[:-5], "origin": "factory", "available": True,
                        "categories": [[cat]], "rights": {"level": "free"}})
        cats.setdefault(cat, []).append(pid)
m = {"connector": 1, "plugin": {"name": "Odin 2", "id": "com.TheWaveWarden.Odin2", "version": "2.4.1"},
     "revision": "odin2-2.4.1-factory", "presets": presets,
     "collections": [{"id": f"category/{c}", "name": c, "kind": "factory", "presetIds": ids} for c, ids in cats.items()]}
out = os.path.join(os.path.dirname(__file__), '..', 'connector', 'odin2.manifest.json')
json.dump(m, open(out, 'w'), indent=1, ensure_ascii=False)
print(len(presets), 'presets ->', os.path.relpath(out))
