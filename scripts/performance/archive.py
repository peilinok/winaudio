"""Archive completed measurement directories without changing their raw inputs."""
import argparse
import csv
import gzip
import hashlib
import io
import json
from pathlib import Path


def sha(data):
    return hashlib.sha256(data).hexdigest()


def process_summary(data, warmup):
    rows = list(csv.DictReader(io.StringIO(data.decode("utf-8-sig"))))
    rows = [{k: float(v) for k, v in row.items()} for row in rows
            if float(row["elapsed_s"]) >= warmup]
    if len(rows) < 2:
        raise ValueError("Not enough post-warmup process samples")
    first, last = rows[0], rows[-1]
    seconds = last["elapsed_s"] - first["elapsed_s"]
    return {"samples": len(rows), "seconds": seconds,
            "cpu_ms_per_s": (last["cpu_ms"] - first["cpu_ms"]) / seconds,
            **{field: {"first": first[field], "last": last[field],
                       "minimum": min(r[field] for r in rows),
                       "maximum": max(r[field] for r in rows)}
               for field in ("private_bytes", "working_set")}}


def archive(source, destination):
    destination.mkdir(parents=True, exist_ok=False)
    manifest, runs = {}, []
    for folder in sorted(source.iterdir()):
        if not folder.is_dir() or not (folder / "config.json").exists():
            continue
        config = json.loads((folder / "config.json").read_text(encoding="utf-8-sig"))
        run = {"name": folder.name, "config": config}
        summary = folder / "summary.json"
        if summary.exists():
            parsed = json.loads(summary.read_text(encoding="utf-8-sig"))
            if not parsed["run_complete"]:
                raise ValueError("Incomplete measurement: " + str(folder))
            run["groups"] = [g for g in parsed["groups"] if g["stage"] != "warmup"]
            run["raw_frame_sha256"] = parsed["sha256"]
        elif config["configuration"].get("mode") != "off":
            raise ValueError("Missing completed summary: " + str(folder))
        run["process"] = process_summary((folder / "process.csv").read_bytes(),
                                         config["configuration"]["warmup"])
        for path in sorted(folder.iterdir()):
            if path.name not in ("config.json", "summary.json", "process.csv", "frames.txt",
                                 "fixture-input.jsonl", "winaudio.log", "fixture-export.jsonl"):
                continue
            original = path.read_bytes()
            # Only normalize the review copies of textual configuration and CSV.
            data = (original.decode("utf-8-sig").replace("\r\n", "\n").encode("utf-8")
                    if path.suffix in (".json", ".csv") else original)
            compress = path.suffix not in (".json", ".csv")
            name = folder.name + "-" + path.name + (".gz" if compress else "")
            # Identical fixed inputs share one artifact; the manifest retains each source.
            if path.name == "fixture-input.jsonl":
                name = "input-" + sha(data) + ".jsonl.gz"
            target = destination / name
            if not target.exists():
                if compress:
                    with target.open("wb") as stream:
                        with gzip.GzipFile(filename="", mode="wb", fileobj=stream, mtime=0) as zipped:
                            zipped.write(data)
                    if gzip.decompress(target.read_bytes()) != data:
                        raise ValueError("Archive round-trip failed")
                else:
                    target.write_bytes(data)
            manifest[str(path)] = {"artifact": name, "source_sha256": sha(original),
                                   "content_sha256": sha(data),
                                   "artifact_sha256": sha(target.read_bytes())}
        runs.append(run)
    if not runs:
        raise ValueError("No measurements found")
    for name, value in (("runs.json", runs), ("manifest.json", manifest)):
        (destination / name).write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n",
                                         encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    archive(args.source, args.destination)
