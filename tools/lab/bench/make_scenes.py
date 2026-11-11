"""Writes the benchmark's standard scenes (scenes/*.json) from the descriptions below.

The scenes use nothing but what every VaM 1.x installation has built in (its people, hair, clothing, lights, shapes)
plus the motion plugin beside them (scenes/BenchMotion.cs), so they can be published and load anywhere. What they are
meant to stand for is said with each one; what they cannot stand for is in ../../../docs/benchmarks/README.md.

A scene file names its plugin by a path under the VaM folder: Saves/scene/vamkit-bench/BenchMotion.cs. To open a
scene by hand, copy the scenes folder to <VaM>/Saves/scene/vamkit-bench/. bench.py stages them itself.

    python tools/lab/bench/make_scenes.py          writes scenes/*.json again (the files are committed)
"""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
PLUGIN = "Saves/scene/vamkit-bench/BenchMotion.cs"


def xyz(x, y, z):
    return {"x": str(x), "y": str(y), "z": str(z)}


def placed(atom, position, rotation=(0, 0, 0)):
    atom.update(position=xyz(*position), rotation=xyz(*rotation), containerPosition=xyz(*position), containerRotation=xyz(*rotation))
    return atom


# The one built-in hair made of simulated strands (about 208,000 segments). Every other built-in hair is a mesh:
# it costs little, is not simulated strand by strand and is not what people wear in real scenes.
STRANDS = "SimV2 Hair"


def person(uid, character, clothing, position, turn=0, speed=0.6, size=1.0, phase=0.0, hair=STRANDS):
    """One of VaM's built-in people with strand hair and built-in clothing, moved by the motion plugin."""
    geometry = {"id": "geometry", "character": character,
                "clothing": [{"id": name, "enabled": "true"} for name in clothing],
                "hair": [{"id": hair, "enabled": "true"}]}
    return placed({"id": uid, "on": "true", "type": "Person", "storables": [
        geometry,
        {"id": "PluginManager", "plugins": {"plugin#0": PLUGIN}},
        {"id": "plugin#0_BenchMotion", "Speed": str(speed), "Size": str(size), "Phase": str(phase)}]}, position, (0, turn, 0))


def light(uid, kind, position, aim, intensity, shadows, colour=None, spot_angle=70, light_range=10):
    storable = {"id": "Light", "type": kind, "intensity": str(intensity), "range": str(light_range), "shadowsOn": "true" if shadows else "false"}
    if kind == "Spot":
        storable["spotAngle"] = str(spot_angle)
    if colour:
        storable["color"] = {"h": str(colour[0]), "s": str(colour[1]), "v": str(colour[2])}
    return placed({"id": uid, "on": "true", "type": "InvisibleLight", "storables": [storable]}, position, aim)


def shape(uid, kind, position, scale=1.0, rotation=(0, 0, 0)):
    return placed({"id": uid, "on": "true", "type": kind, "storables": [{"id": "scale", "scale": str(scale)}]}, position, rotation)


def scene(atoms):
    fixed = [{"id": "CoreControl", "on": "true", "type": "CoreControl", "storables": []},
             placed({"id": "PlayerNavigationPanel", "on": "true", "type": "PlayerNavigationPanel", "storables": []}, (0, 0, 0))]
    return {"playerNavCollider": "PlayerNavigationPanel:Floor", "worldScale": "1", "useSceneLoadPosition": "false", "atoms": fixed + atoms}


# The viewer stands where VaM puts it: eyes at (0, 1.6, 1.5), looking along -z. A person at the origin faces the viewer.
SCENES = {
    # A close look at one person: skin, eyes and long strand hair fill the view; three lights, two with shadows.
    # Stands for: a portrait scene, the most common way a look is judged.
    "portrait": scene([
        person("Person", "Female 3", ["Tank Top", "Shorts"], (0, 0, 0.55), speed=0.25, size=0.3),
        light("Key", "Spot", (0.9, 1.9, 1.6), (15, 205, 0), 1.6, True),
        light("Fill", "Point", (-1.1, 1.5, 1.4), (0, 0, 0), 0.5, False),
        light("Rim", "Spot", (-0.8, 2.1, -0.9), (25, 40, 0), 1.4, True, colour=(0.08, 0.35, 1)),
    ]),
    # One person dancing in simulated clothing with strand hair, seen whole; four lights, three with shadows.
    # Stands for: a solo dance scene, where hair and cloth physics and motion vectors matter.
    "dance": scene([
        person("Person", "Female 7", ["Seductress Dress Sim", "Stockings Sim", "Harli Heels"], (0, 0, -0.6)),
        light("Key", "Spot", (1.3, 2.4, 1.8), (22, 205, 0), 1.7, True),
        light("Fill", "Spot", (-1.6, 1.8, 1.5), (10, 150, 0), 0.8, True),
        light("Rim", "Spot", (0.9, 2.3, -2.2), (25, -25, 0), 1.5, True, colour=(0.58, 0.4, 1)),
        light("Top", "Point", (0, 2.8, -0.6), (0, 0, 0), 0.4, False),
    ]),
    # Two people close together, both moving, both with strand hair and simulated clothing; five lights, four with shadows.
    # Stands for: the heavy two-person scene, the case the frame budget is planned around.
    "duo": scene([
        person("PersonA", "Female 5", ["Heat Up Skirt Sim", "Heat Up Top", "Harli Heels"], (-0.32, 0, -0.2), turn=25),
        person("PersonB", "Female 12", ["DATM Pants Sim", "DATM Jumper Sim"], (0.32, 0, -0.2), turn=-25, phase=0.5),
        light("Key", "Spot", (1.2, 2.3, 1.7), (20, 205, 0), 1.6, True),
        light("Fill", "Spot", (-1.5, 1.9, 1.5), (12, 150, 0), 0.9, True),
        light("RimLeft", "Spot", (-1.2, 2.2, -1.8), (25, 35, 0), 1.3, True, colour=(0.95, 0.5, 1)),
        light("RimRight", "Spot", (1.2, 2.2, -1.8), (25, -35, 0), 1.3, True, colour=(0.55, 0.5, 1)),
        light("Top", "Point", (0, 2.8, -0.2), (0, 0, 0), 0.4, False),
    ]),
    # Three dancers (strand hair, simulated clothing) further away on a set made of many objects and coloured lights
    # (eight lights, four with shadows).
    # Stands for: a stage scene, where the number of objects and lights weighs more than any single person.
    "stage": scene(
        [person("Dancer%d" % (i + 1), character, clothing, (x, 0, -2.0), speed=0.6, phase=i / 3.0)
         for i, (character, clothing, x) in enumerate((
             ("Female 2", ["SummerGirl Shirt Sim", "SummerGirl Shorts Sim"], -0.95),
             ("Female 9", ["Heatwave Shirt Sim", "Casual Denim Jeans"], 0.0),
             ("Female 14", ["Alphakini Bra Sim", "Heat Up Skirt Sim"], 0.95)))]
        + [light("Key", "Spot", (0, 3.0, 2.0), (25, 180, 0), 1.6, True, spot_angle=80, light_range=14),
           light("SideLeft", "Spot", (-3.0, 2.6, 0.5), (20, 125, 0), 1.2, True, colour=(0.0, 0.7, 1), light_range=12),
           light("SideRight", "Spot", (3.0, 2.6, 0.5), (20, 235, 0), 1.2, True, colour=(0.6, 0.7, 1), light_range=12),
           light("Back", "Spot", (0, 3.2, -5.0), (30, 0, 0), 1.4, True, colour=(0.8, 0.5, 1), light_range=12)]
        + [light("Wash%d" % (i + 1), "Point", (x, 2.2, -3.2), (0, 0, 0), 0.5, False, colour=(h, 0.8, 1))
           for i, (x, h) in enumerate(((-2.4, 0.33), (-0.8, 0.5), (0.8, 0.12), (2.4, 0.9)))]
        + [shape("Riser%d" % (i + 1), "Cube", (-3.0 + 0.75 * i, 0.2, -3.6), scale=0.4) for i in range(9)]
        + [shape("Globe%d" % (i + 1), "Sphere", (-3.2 + 0.8 * i, 2.6 + 0.15 * (i % 3), -4.2), scale=0.35) for i in range(9)]
        + [shape("Post%d" % (i + 1), "Capsule", (x, 1.0, z), scale=0.5) for i, (x, z) in enumerate(
            ((-3.4, -1.0), (-3.4, -2.6), (3.4, -1.0), (3.4, -2.6), (-2.0, -4.6), (2.0, -4.6)))]
        + [shape("Block%d" % (i + 1), "Cube", (-2.6 + 1.3 * (i % 5), 0.15, 0.2 - 0.9 * (i // 5)), scale=0.3, rotation=(0, 17 * i, 0))
           for i in range(10) if i not in (1, 2, 3, 6, 7, 8)]),
}


def main():
    folder = HERE / "scenes"
    folder.mkdir(exist_ok=True)
    for name, content in SCENES.items():
        path = folder / f"bench-{name}.json"
        path.write_text(json.dumps(content, indent=3) + "\n", encoding="utf-8", newline="\n")
        people = sum(a["type"] == "Person" for a in content["atoms"])
        lights = [a for a in content["atoms"] if a["type"] == "InvisibleLight"]
        shadowed = sum(a["storables"][0]["shadowsOn"] == "true" for a in lights)
        print(f"{path.name}: {len(content['atoms'])} atoms, {people} people, {len(lights)} lights ({shadowed} with shadows)")


if __name__ == "__main__":
    main()
