"""Train signed-slope experts and bake the two stone-wall atlases."""
import argparse
import hashlib
import json
from pathlib import Path

import neural_stonewall as neural
import numpy as np


SHALLOW = (-3.2, 3.2)
GRAZING_NEGATIVE = (-12.0, -2.8)


def train_expert(project, teacher, work, expert, focus_grazing):
    slope_x, slope_y = {
        "shallow": (SHALLOW, SHALLOW),
        "grazing-positive": (neural.GRAZING_X, neural.GRAZING_Y),
        "grazing-negative": (GRAZING_NEGATIVE, neural.GRAZING_Y),
    }[expert]
    dataset = work / expert / "dataset"
    neural.prepare_angular_dataset(project, teacher, dataset, slope_x, slope_y, focus_grazing)
    if focus_grazing:
        previous = project / "Assets/SilPOMPlanarAB/NeuralStoneWall"
        name = "grazing_model.json" if expert == "grazing-positive" else "grazing_negative_model.json"
        neural.train(dataset, work / expert, initial_model=previous / name, steps=45000)
    else:
        neural.train(dataset, work / expert)


def pack(project, work):
    previous = project / "Assets/SilPOMPlanarAB/NeuralStoneWall"
    shallow_model = work / "shallow/model.json"
    if not shallow_model.exists():
        shallow_model = previous / "shallow_bidirectional_model.json"
    negative_model = work / "grazing-negative/model.json"
    positive_model = work / "grazing-positive/model.json"
    if not positive_model.exists():
        positive_model = previous / "grazing_model.json"
    shallow = neural.evaluate_atlas(shallow_model, x_slopes=32)
    # Preserve the existing positive expert and its 16 angle samples.
    positive = neural.evaluate_atlas(positive_model)
    negative = neural.evaluate_atlas(negative_model)
    grazing = np.concatenate((positive, negative), axis=1)
    output = work / "packed"
    output.mkdir(exist_ok=True)
    report = {"uvResolution": 128, "format": "R32_FLOAT", "caches": {}}
    for name, atlas, domains, models in (
        ("ShallowBidirectionalHitCache", shallow, [(SHALLOW, SHALLOW, 32)], [shallow_model]),
        ("GrazingBidirectionalHitCache", grazing,
         [(neural.GRAZING_X, neural.GRAZING_Y, 16), (GRAZING_NEGATIVE, neural.GRAZING_Y, 16)],
         [positive_model, negative_model]),
    ):
        payload = neural.atlas_dds(atlas)
        (output / (name + ".dds")).write_bytes(payload)
        report["caches"][name] = {
            "size": [atlas.shape[1], atlas.shape[0]], "bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "experts": [{"slopeX": x, "slopeY": y, "columns": columns,
                         "modelSha256": hashlib.sha256(model.read_bytes()).hexdigest()}
                        for (x, y, columns), model in zip(domains, models)],
        }
    existing = previous / "bidirectional_cache.json"
    report["training"] = json.loads(existing.read_text())["training"] if existing.exists() else {}
    for expert in ("shallow", "grazing-negative", "grazing-positive"):
        training = work / expert / "training.json"
        if training.exists():
            report["training"][expert] = json.loads(training.read_text())
    (output / "bidirectional_cache.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("shallow", "grazing-positive", "grazing-negative", "pack"))
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--teacher", type=Path)
    parser.add_argument("--focus-grazing", action="store_true", help="Oversample and fine-tune around 80 degrees")
    args = parser.parse_args()
    if args.mode == "pack":
        pack(args.project, args.work)
    else:
        if not args.teacher:
            parser.error("--teacher is required for training")
        if args.focus_grazing and args.mode == "shallow":
            parser.error("--focus-grazing requires a grazing expert")
        train_expert(args.project, args.teacher, args.work, args.mode, args.focus_grazing)
