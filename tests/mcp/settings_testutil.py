"""Helpers shared by the settings group's tests (modifiers, layer ranges, settings_set, presets, filaments)."""

from __future__ import annotations

import math

from conftest import cube


def approx(a, b, tol=0.02):
    """Element-wise comparison of numbers or nested lists."""
    if isinstance(a, (list, tuple)):
        return len(a) == len(b) and all(approx(x, y, tol) for x, y in zip(a, b))
    return math.isclose(a, b, abs_tol=tol)


def box_center(box):
    return [(box["min"][i] + box["max"][i]) / 2 for i in range(3)]


def box_size(box):
    return [box["max"][i] - box["min"][i] for i in range(3)]


def scene_signature(client):
    """The scene without the fields that change on every call (rev)."""
    scene = client.tool("scene_get", detail="full")
    scene["project"].pop("rev", None)
    scene["project"].pop("dirty", None)
    for plate in scene["plates"]:
        plate.pop("sliced", None)
        for obj in plate["objects"]:
            for inst in obj["instances"]:
                inst.pop("outside_plate", None)
    return scene


def undo(client, steps=1):
    client.tool("history", action="undo", steps=steps)


def revert_journal(client, seqs):
    client.tool("history", action="revert", journal_seq=list(seqs))


def the_cube(client):
    return cube(client)


def volume_of(obj, volume_id):
    for volume in obj["volumes"]:
        if volume["id"] == volume_id:
            return volume
    raise KeyError(volume_id)
