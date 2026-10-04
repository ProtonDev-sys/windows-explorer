#!/usr/bin/env python3
"""Headless native screenshot checks against pinned online Windows 10 references.

Nothing in this module opens a window. PNGs come from the C++ PrivateDesktop /
PrintWindow capture path. References/diffs stay under ignored artifacts.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import urllib.parse
import urllib.request

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "tests" / "visual" / "windows10-reference.json"
ALLOWED_REFERENCE_HOSTS = {
    "download.microsoft.com", "support.microsoft.com",
    "techpilipinas.com", "cdn.mos.cms.futurecdn.net",
    "www.pcassistonline.co.uk",
    "techcult.com",
    "assets.techrepublic.com", "www.audioprecision.com",
    "fileinfo.com",
}
MAXIMUM_DOWNLOAD = 64 * 1024 * 1024


def validate_manifest(manifest: dict) -> None:
    if manifest.get("schema") != 1:
        raise ValueError("Unsupported visual reference manifest version")
    ids: set[str] = set()
    for source in manifest.get("sources", []):
        if source.get("kind") not in ("image", "pdf"):
            raise ValueError("Reference source needs a supported image/pdf kind")
        records = [source, *source.get("images", [])]
        for record in records:
            identifier = record.get("id")
            if not isinstance(identifier, str) or not identifier or identifier in ids:
                raise ValueError("Reference identifiers must be nonempty and unique")
            ids.add(identifier)
            filename = record.get("file")
            if not isinstance(filename, str) or Path(filename).name != filename or any(ch in filename for ch in "\\/:"):
                raise ValueError("Reference filename must stay within its artifact directory")
            if not re.fullmatch(r"[0-9a-f]{64}", record.get("sha256", "")):
                raise ValueError("Reference requires a pinned SHA-256")
    records = reference_records(manifest)
    scenes: set[str] = set()
    for scene in manifest.get("scenes", []):
        name = scene.get("name", "")
        if not re.fullmatch(r"[A-Za-z]+", name) or name.casefold() in scenes:
            raise ValueError("Scene names must be nonempty and unique")
        scenes.add(name.casefold())
        if scene.get("reference") not in records or not scene.get("regions"):
            raise ValueError("Every compared scene needs a declared reference and measured regions")
        if any(type(scene.get(axis)) is not int or not 1 <= scene[axis] <= 8192 for axis in ("width", "height", "layoutDpi")):
            raise ValueError("Every scene needs bounded native dimensions and recorded layout DPI")
        regions: set[str] = set()
        for region in scene["regions"]:
            if not re.fullmatch(r"[A-Za-z0-9-]+", region.get("name", "")):
                raise ValueError("Region names must be safe diagnostic filenames")
            if region["name"] in regions:
                raise ValueError("Region names must be unique within their scene")
            regions.add(region["name"])
            if region.get("actualOrigin", "window") not in ("window", "client", "ribbon", "widget"):
                raise ValueError("Region origin must be measured window/client/Ribbon/widget geometry")
            if region.get("actualOrigin") == "widget":
                selector = region.get("actualWidget", {})
                if type(selector.get("id")) is not int or not isinstance(selector.get("class"), str) or not selector["class"]:
                    raise ValueError("Widget origin requires an explicit native control ID and class")
                if "expectedHeight" in selector and (type(selector["expectedHeight"]) is not int or selector["expectedHeight"] <= 0):
                    raise ValueError("Native widget expected height must be positive integer pixels")
            if region.get("referenceId", scene["reference"]) not in records:
                raise ValueError("Region-specific reference is not pinned in the manifest")
            if region.get("captureVariant", "primary") not in ("primary", "modernQuickAccess"):
                raise ValueError("Region capture variant must name a declared native source-state fixture")
            if "condition" in region:
                condition = region["condition"]
                if condition.get("globalSetting") != "fileNameExtensions" or type(condition.get("equals")) is not bool:
                    raise ValueError("Conditional regions require a declared boolean native global setting")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_file(path: Path, expected_hash: str) -> None:
    if not path.is_file():
        raise ValueError(f"Missing reference: {path}; run --fetch first")
    if sha256(path) != expected_hash:
        raise ValueError(f"Reference SHA-256 mismatch: {path}")


def download(url: str, output: Path, expected_hash: str) -> None:
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "https" or parsed.hostname not in ALLOWED_REFERENCE_HOSTS:
        raise ValueError("Reference URL must be HTTPS on a declared screenshot origin")
    if output.exists():
        verify_file(output, expected_hash)
        return
    request = urllib.request.Request(url, headers={"User-Agent": "WindowsExplorer-Headless-ReferenceCheck/1.0"})
    with urllib.request.urlopen(request, timeout=45) as response:
        final = urllib.parse.urlparse(response.url)
        if final.scheme != "https" or final.hostname not in ALLOWED_REFERENCE_HOSTS:
            raise ValueError("Reference download redirected outside declared screenshot origins")
        data = response.read(MAXIMUM_DOWNLOAD + 1)
    if len(data) > MAXIMUM_DOWNLOAD:
        raise ValueError("Reference exceeds bounded download size")
    if hashlib.sha256(data).hexdigest() != expected_hash:
        raise ValueError(f"Downloaded reference SHA-256 mismatch: {url}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as stream:
        stream.write(data)


def reference_records(manifest: dict) -> dict[str, dict]:
    records: dict[str, dict] = {}
    for source in manifest["sources"]:
        if source["kind"] == "image":
            records[source["id"]] = source
        else:
            for record in source["images"]:
                records[record["id"]] = {**record, "sourceUrl": source["url"], "sourcePage": record["page"]}
    return records


def fetch_references(manifest: dict, directory: Path) -> dict:
    if directory.resolve() != (ROOT / "artifacts" / "reference").resolve():
        raise ValueError("Copyrighted reference downloads must stay in ignored artifacts/reference")
    directory.mkdir(parents=True, exist_ok=True)
    for source in manifest["sources"]:
        output = directory / source["file"]
        download(source["url"], output, source["sha256"])
        if source["kind"] == "pdf":
            import fitz
            with fitz.open(output) as document:
                for image in source["images"]:
                    path = directory / image["file"]
                    if path.exists():
                        verify_file(path, image["sha256"])
                        continue
                    page = document[image["page"] - 1]
                    if image["xref"] not in [item[0] for item in page.get_images()]:
                        raise ValueError("Pinned screenshot xref is absent from the recorded source page")
                    data = document.extract_image(image["xref"])["image"]
                    if hashlib.sha256(data).hexdigest() != image["sha256"]:
                        raise ValueError("Extracted screenshot SHA-256 mismatch")
                    with path.open("xb") as stream:
                        stream.write(data)
    provenance = {"schema": 1, "references": reference_records(manifest), "distribution": "ignored research intermediates only"}
    (directory / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    return provenance


def checked_rectangle(value: list[int], width: int, height: int) -> tuple[int, int, int, int]:
    if len(value) != 4 or any(type(number) is not int for number in value):
        raise ValueError("Crop/mask must be four integer pixel coordinates")
    left, top, right, bottom = value
    if left < 0 or top < 0 or right <= left or bottom <= top or right > width or bottom > height:
        raise ValueError(f"Crop/mask outside image: {value} vs {width}x{height}")
    return left, top, right, bottom


def edge_map(rgb: np.ndarray) -> np.ndarray:
    luminance = rgb.astype(np.float32) @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    horizontal = np.zeros_like(luminance)
    vertical = np.zeros_like(luminance)
    horizontal[:, 1:] = np.abs(np.diff(luminance, axis=1))
    vertical[1:, :] = np.abs(np.diff(luminance, axis=0))
    return np.maximum(horizontal, vertical) >= 24


def dilate(edges: np.ndarray, radius: int) -> np.ndarray:
    if radius < 0 or radius > 4:
        raise ValueError("Edge tolerance must be between 0 and 4 pixels")
    if not radius:
        return edges
    result = np.zeros_like(edges)
    padded = np.pad(edges, radius)
    height, width = edges.shape
    for y in range(2 * radius + 1):
        for x in range(2 * radius + 1):
            result |= padded[y:y + height, x:x + width]
    return result


def image_metrics(reference: np.ndarray, actual: np.ndarray, mask: np.ndarray, thresholds: dict) -> dict:
    if reference.shape != actual.shape or reference.ndim != 3 or reference.shape[-1] != 3:
        raise ValueError("Comparisons require identical RGB crop dimensions; no silent resampling")
    if mask.shape != reference.shape[:2] or not np.any(mask):
        raise ValueError("Comparison mask has no measured pixels")
    delta = np.abs(reference.astype(np.int16) - actual.astype(np.int16))
    measured = delta[mask]
    reference_edges = edge_map(reference) & mask
    actual_edges = edge_map(actual) & mask
    radius = thresholds["edgeTolerancePixels"]
    # Don't turn excluded text/content at mask boundaries into a match.
    valid_edges = ~dilate(~mask, radius)
    reference_edges &= valid_edges
    actual_edges &= valid_edges
    reference_count = int(np.count_nonzero(reference_edges))
    actual_count = int(np.count_nonzero(actual_edges))
    recall = float(np.count_nonzero(reference_edges & dilate(actual_edges, radius)) / reference_count) if reference_count else (1.0 if not actual_count else 0.0)
    precision = float(np.count_nonzero(actual_edges & dilate(reference_edges, radius)) / actual_count) if actual_count else (1.0 if not reference_count else 0.0)
    edge_f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    result = {
        "measuredPixels": int(np.count_nonzero(mask)),
        "meanAbsoluteRgbError": float(np.mean(measured)),
        "pixelAgreement": float(np.mean(np.max(measured, axis=1) <= thresholds["perChannelTolerance"])),
        "edgeF1": edge_f1,
        "edgePrecision": precision,
        "edgeRecall": recall,
        "referenceEdgePixels": reference_count,
        "actualEdgePixels": actual_count,
        "referenceUniqueColors": unique_rgb_colors(reference[mask]),
        "actualUniqueColors": unique_rgb_colors(actual[mask]),
    }
    result["pixelsPassed"] = result["meanAbsoluteRgbError"] <= thresholds["meanAbsoluteRgbError"] and result["pixelAgreement"] >= thresholds["pixelAgreement"]
    result["geometryPassed"] = result["edgeF1"] >= thresholds["edgeF1"]
    result["passed"] = result["pixelsPassed"] and result["geometryPassed"]
    return result


def unique_rgb_colors(rgb: np.ndarray) -> int:
    """Count exact RGB triples without an expensive three-column row sort."""
    colors = np.asarray(rgb, dtype=np.uint32).reshape(-1, 3)
    packed = colors[:, 0] << 16 | colors[:, 1] << 8 | colors[:, 2]
    return int(np.unique(packed).size)


def capture_invariants(capture: dict, image: Image.Image, require_ribbon: bool = False) -> list[str]:
    failures = []
    for key in ("headless", "privateDesktop", "inputDesktopUnchanged", "printWindowSucceeded"):
        if capture.get(key) is not True:
            failures.append(f"Native capture invariant is absent/false: {key}")
    if capture.get("renderer") != "native-PrintWindow-WIC":
        failures.append("Capture did not use actual native PrintWindow/WIC rendering")
    if capture.get("visibleInputDesktopWindows") is not False:
        failures.append("Capture did not prove no process window on the input desktop")
    if image.size != (capture.get("width"), capture.get("height")):
        failures.append("PNG dimensions differ from native capture inventory")
    if capture.get("unpaintedFraction", 1) > 0.001 or capture.get("uniqueColors", 0) < 12 or capture.get("visibleChildren", 0) < 1:
        failures.append("Native capture appears blank, unpainted, or has no actual controls")
    if capture.get("windowDpi") != capture.get("layoutDpi"):
        failures.append("Requested layout DPI differs from actual native window DPI; scaling is not native DPI evidence")
    rgb = np.asarray(image.convert("RGB"))
    if unique_rgb_colors(rgb) < 12:
        failures.append("Actual PNG has too few colors to show rendered native widgets")
    # A colorful ItemsView cannot prove the native Ribbon commands painted.
    # Verify the expanded command band independently of caption/tab text. The
    # HWND inventory supplies its actual geometry; these are unscaled pixels.
    ribbon = [widget for widget in capture.get("widgets", []) if
              widget.get("class") == "UIRibbonCommandBar" and widget.get("visible")]
    if require_ribbon and not ribbon:
        failures.append("Required native Ribbon command-bar HWND is absent")
    if ribbon:
        if len(ribbon) != 1:
            failures.append("Native Ribbon has no unique visible command-bar HWND")
        else:
            try:
                left, top, right, bottom = checked_rectangle(ribbon[0]["bounds"], *image.size)
                dpi = capture.get("windowDpi")
                if type(dpi) is not int or not 48 <= dpi <= 768:
                    raise ValueError("Native Ribbon lacks a bounded actual window DPI")
                scale = dpi / 96
                # A minimized Ribbon contains only the native tab strip. An
                # expanded Ribbon is at least 60 logical pixels high.
                if bottom - top >= round(60 * scale):
                    band = rgb[top + round(26 * scale):bottom - round(6 * scale),
                               left + round(2 * scale):right - round(2 * scale)]
                    # High contrast can legitimately paint binary text/icons.
                    # Require actual command content edges as well as pixels,
                    # instead of rejecting a two-color but fully rendered band.
                    if not band.size or unique_rgb_colors(band) < 2 or np.count_nonzero(edge_map(band)) < 60:
                        failures.append("Expanded native Ribbon command band is blank or unpainted")
            except (ValueError, TypeError) as error:
                failures.append(f"Invalid native Ribbon capture geometry: {error}")
    # The owned collapse button is outside the native Framework command body.
    # A real HWND/name/working click must not conceal an unpainted caret.
    for button in capture.get("widgets", []):
        if button.get("id") != 139 or button.get("class") != "Button" or not button.get("visible"):
            continue
        try:
            left, top, right, bottom = checked_rectangle(button["bounds"], *image.size)
            dpi = capture.get("windowDpi")
            if type(dpi) is not int or not 48 <= dpi <= 768:
                raise ValueError("Collapse control lacks bounded actual window DPI")
            inset = max(1, round(4 * dpi / 96))
            caret = rgb[top + inset:bottom - inset, left + inset:right - inset]
            # Ignore the outer focus/hover frame for this nonblank-content
            # invariant; its actual pixels remain in source comparisons.
            if not caret.size or unique_rgb_colors(caret) < 2 or np.count_nonzero(edge_map(caret)) < 4:
                failures.append("Native Ribbon collapse caret is blank or unpainted")
        except (ValueError, TypeError) as error:
            failures.append(f"Invalid native collapse capture geometry: {error}")
    return failures


def compare_scene(manifest: dict, scene_name: str, references: Path, actual_path: Path,
                  capture_path: Path, output: Path,
                  capture_variants: dict[str, tuple[Path, Path]] | None = None) -> dict:
    scenes = [scene for scene in manifest["scenes"] if scene["name"].casefold() == scene_name.casefold()]
    if len(scenes) != 1:
        raise ValueError("Scene has no unique sourced screenshot baseline")
    scene = scenes[0]
    reference = reference_records(manifest)[scene["reference"]]
    reference_path = references / reference["file"]
    verify_file(reference_path, reference["sha256"])
    reference_image = Image.open(reference_path).convert("RGB")
    actual_image = Image.open(actual_path).convert("RGB")
    capture = json.loads(capture_path.read_text(encoding="utf-8-sig"))
    failures = capture_invariants(capture, actual_image)
    if actual_image.size != (scene["width"], scene["height"]):
        failures.append("Native window dimensions differ from the recorded source scene")
    if capture.get("layoutDpi") != scene["layoutDpi"]:
        failures.append("Native layout DPI differs from the recorded comparison scene")
    native_sources = {"primary": {"image": actual_image, "capture": capture,
                       "actualSha256": sha256(actual_path), "captureSha256": sha256(capture_path)}}
    for name, (variant_image_path, variant_capture_path) in (capture_variants or {}).items():
        if name != "modernQuickAccess":
            raise ValueError("Unknown native capture variant")
        variant_image = Image.open(variant_image_path).convert("RGB")
        variant_capture = json.loads(variant_capture_path.read_text(encoding="utf-8-sig"))
        variant_failures = capture_invariants(variant_capture, variant_image, require_ribbon=True)
        address_entries = [widget for widget in variant_capture.get("widgets", []) if
                           widget.get("id") == 104 and widget.get("class") == "Edit"]
        if len(address_entries) != 1 or not address_entries[0].get("text", "").casefold().endswith(
                "::{679f85cb-0220-4080-b29b-5540cc05aab6}"):
            variant_failures.append("Modern chrome variant did not prove the actual native Quick Access namespace")
        if variant_capture.get("layoutDpi") != scene["layoutDpi"]:
            variant_failures.append("Native layout DPI differs from the recorded comparison scene")
        failures.extend(f"{name}: {failure}" for failure in variant_failures)
        native_sources[name] = {"image": variant_image, "capture": variant_capture,
                               "actualSha256": sha256(variant_image_path), "captureSha256": sha256(variant_capture_path)}
    result = {"schema": 1, "scene": scene["name"], "source": reference, "referenceSha256": reference["sha256"],
              "actualSha256": sha256(actual_path), "nativeWindowDpi": capture.get("windowDpi"),
              "requestedLayoutDpi": capture.get("layoutDpi"), "dpiEvidence": scene["dpiEvidence"],
              "nativeCaptureFailures": failures, "regions": [], "thresholds": manifest["thresholds"],
              "comparisonScope": manifest["limits"]}
    result["nativeCaptureVariants"] = {name: {"actualSha256": source["actualSha256"],
        "captureSha256": source["captureSha256"], "nativeWindowDpi": source["capture"].get("windowDpi"),
        "headless": source["capture"].get("headless"), "privateDesktop": source["capture"].get("privateDesktop")}
        for name, source in native_sources.items()}
    output.mkdir(parents=True, exist_ok=True)
    for region in scene["regions"]:
        record = {"name": region["name"], "kind": region["kind"], "reason": region["reason"],
                  "masks": region.get("masks", []), "maskReasons": region.get("maskReasons", []), "applicable": True}
        try:
            variant_name = region.get("captureVariant", "primary")
            if variant_name not in native_sources:
                raise ValueError(f"Required native source-state capture variant is absent: {variant_name}")
            native_source = native_sources[variant_name]
            region_capture = native_source["capture"]
            region_image = native_source["image"]
            record["actualCaptureVariant"] = variant_name
            record["actualSha256"] = native_source["actualSha256"]
            record["nativeCaptureSha256"] = native_source["captureSha256"]
            if "condition" in region:
                condition = region["condition"]
                setting = region_capture.get("globalSettings", {}).get(condition["globalSetting"], {})
                if setting.get("readSucceeded") is not True or setting.get("matchesOs") is not True or \
                        type(setting.get("osValue")) is not bool or type(setting.get("ribbonValue")) is not bool or \
                        setting["osValue"] != setting["ribbonValue"] or setting.get("registryValue") not in (0, 1) or \
                        setting["osValue"] != (setting["registryValue"] == 0):
                    raise ValueError("Global checkbox comparison lacks matching actual OS and native Ribbon readbacks")
                record["nativeGlobalSetting"] = setting
                record["condition"] = condition
                if setting["ribbonValue"] != condition["equals"]:
                    record.update({"applicable": False, "passed": None,
                                   "selectionReason": "Pinned alternative state; actual native OS state selects the other reference"})
                    result["regions"].append(record)
                    continue
            selected_reference = reference_records(manifest)[region.get("referenceId", scene["reference"])]
            selected_image = reference_image
            if selected_reference != reference:
                selected_path = references / selected_reference["file"]
                verify_file(selected_path, selected_reference["sha256"])
                selected_image = Image.open(selected_path).convert("RGB")
            record["referenceSource"] = selected_reference
            reference_rect = checked_rectangle(region["reference"], *selected_image.size)
            actual_rect = list(region["actual"])
            if region.get("actualOrigin", "window") == "client":
                origin = region_capture["clientBounds"]
                actual_rect = [actual_rect[0] + origin[0], actual_rect[1] + origin[1], actual_rect[2] + origin[0], actual_rect[3] + origin[1]]
            elif region.get("actualOrigin") == "ribbon":
                # Native Ribbon extends the client area into the caption for
                # title-bar QAT hosting. GetClientRect's origin can be y=0;
                # measure the real command-bar HWND instead of assuming y=31.
                candidates = [widget for widget in region_capture.get("widgets", [])
                              if widget.get("class") == "UIRibbonCommandBar" and widget.get("visible")]
                if len(candidates) != 1:
                    raise ValueError("Native Ribbon command-bar HWND has no unique visible inventory entry")
                origin = candidates[0]["bounds"]
                actual_rect = [actual_rect[0] + origin[0], actual_rect[1] + origin[1], actual_rect[2] + origin[0], actual_rect[3] + origin[1]]
            elif region.get("actualOrigin") == "widget":
                selector = region["actualWidget"]
                candidates = [widget for widget in region_capture.get("widgets", []) if widget.get("visible") and
                              widget.get("id") == selector["id"] and widget.get("class") == selector["class"]]
                if len(candidates) != 1:
                    raise ValueError("Native widget has no unique visible ID/class inventory entry")
                origin = checked_rectangle(candidates[0]["bounds"], *region_image.size)
                width, height = origin[2] - origin[0], origin[3] - origin[1]
                checked_rectangle(actual_rect, width, height)
                if "expectedHeight" in selector and height != selector["expectedHeight"]:
                    raise ValueError(f"Native widget height differs: expected {selector['expectedHeight']}, actual {height}")
                record["nativeWidget"] = candidates[0]
                actual_rect = [actual_rect[0] + origin[0], actual_rect[1] + origin[1], actual_rect[2] + origin[0], actual_rect[3] + origin[1]]
            actual_rect = checked_rectangle(actual_rect, *region_image.size)
            first = selected_image.crop(reference_rect)
            second = region_image.crop(actual_rect)
            if first.size != second.size:
                raise ValueError(f"Region dimensions differ: reference {first.size}, actual {second.size}")
            mask = np.ones((first.height, first.width), dtype=bool)
            for rectangle in region.get("masks", []):
                x1, y1, x2, y2 = checked_rectangle(rectangle, first.width, first.height)
                mask[y1:y2, x1:x2] = False
            first_rgb = np.asarray(first)
            second_rgb = np.asarray(second)
            record.update(image_metrics(first_rgb, second_rgb, mask, manifest["thresholds"]))
            record["referenceCrop"] = list(reference_rect)
            record["actualCrop"] = list(actual_rect)
            # Differences/side-by-side contain copyrighted reference portions;
            # these are local ignored artifacts, never source/release assets.
            delta = np.max(np.abs(first_rgb.astype(np.int16) - second_rgb.astype(np.int16)), axis=2)
            heatmap = np.zeros_like(first_rgb)
            heatmap[..., 0] = delta
            heatmap[..., 1] = np.where(mask, 0, 90)
            heatmap[..., 2] = np.where(mask, 0, 90)
            Image.fromarray(heatmap).save(output / f"{scene['name']}-{region['name']}-diff.png")
            panel = Image.new("RGB", (first.width, first.height * 2), "white")
            panel.paste(first, (0, 0)); panel.paste(second, (0, first.height))
            draw = ImageDraw.Draw(panel)
            for rectangle in region.get("masks", []):
                draw.rectangle(rectangle, outline="magenta", width=1)
                draw.rectangle([rectangle[0], rectangle[1] + first.height, rectangle[2], rectangle[3] + first.height], outline="magenta", width=1)
            panel.save(output / f"{scene['name']}-{region['name']}-comparison.png")
        except (ValueError, KeyError) as error:
            record.update({"passed": False, "error": str(error)})
        result["regions"].append(record)
    measured_regions = [item for item in result["regions"] if item["applicable"]]
    result["regionsCompared"] = len(measured_regions)
    result["passed"] = not failures and bool(measured_regions) and all(item["passed"] for item in measured_regions)
    (output / f"{scene['name']}-comparison.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--references", type=Path, default=ROOT / "artifacts" / "reference")
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--validate-capture", action="store_true",
                        help="Validate actual PNG/native HWND inventory without source comparison")
    parser.add_argument("--require-ribbon", action="store_true",
                        help="Require a visible native Ribbon for application captures")
    parser.add_argument("--scene")
    parser.add_argument("--actual", type=Path)
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--chrome-actual", type=Path,
                        help="Actual native Quick Access PNG for declared modern chrome regions")
    parser.add_argument("--chrome-capture", type=Path,
                        help="Native private-desktop inventory belonging to --chrome-actual")
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts" / "visual-comparison")
    args = parser.parse_args()
    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8-sig"))
        validate_manifest(manifest)
        if args.fetch:
            records = fetch_references(manifest, args.references)
            print(f"Verified {len(records['references'])} pinned Windows 10 screenshot references under {args.references}")
        if args.validate_capture:
            if not args.actual or not args.capture:
                parser.error("--validate-capture requires --actual and --capture")
            capture = json.loads(args.capture.read_text(encoding="utf-8-sig"))
            with Image.open(args.actual) as actual:
                failures = capture_invariants(capture, actual, require_ribbon=args.require_ribbon)
            print(json.dumps({"nativeCapturePassed": not failures, "captureFailures": failures}))
            if failures:
                return 1
        if args.scene:
            if not args.actual or not args.capture:
                parser.error("--scene requires --actual and --capture")
            if not args.output.resolve().is_relative_to((ROOT / "artifacts").resolve()):
                raise ValueError("Reference comparison images must stay under ignored artifacts")
            if bool(args.chrome_actual) != bool(args.chrome_capture):
                parser.error("--chrome-actual and --chrome-capture are required together")
            variants = {"modernQuickAccess": (args.chrome_actual, args.chrome_capture)} if args.chrome_actual else None
            result = compare_scene(manifest, args.scene, args.references, args.actual, args.capture, args.output, variants)
            print(json.dumps({"scene": result["scene"], "passed": result["passed"], "captureFailures": result["nativeCaptureFailures"],
                              "regions": [{key: record.get(key) for key in ["name", "passed", "meanAbsoluteRgbError", "pixelAgreement", "edgeF1", "error"]} for record in result["regions"]]}, indent=2))
            return 0 if result["passed"] else 1
        if not args.fetch and not args.validate_capture:
            parser.error("Use --fetch, --validate-capture and/or --scene")
        return 0
    except (OSError, ValueError, KeyError, ImportError) as error:
        print(f"Visual check failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
