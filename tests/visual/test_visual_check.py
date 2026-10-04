"""No GUI or network: verify the screenshot comparison cannot hide mismatches."""
from pathlib import Path
import importlib.util
import copy
import json
import tempfile
import unittest

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("visual_check", ROOT / "scripts" / "visual_check.py")
visual = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(visual)


class MetricsTests(unittest.TestCase):
    def setUp(self):
        self.thresholds = {"meanAbsoluteRgbError": 4.0, "pixelAgreement": 0.98,
                           "edgeF1": 0.9, "perChannelTolerance": 16, "edgeTolerancePixels": 1}
        self.image = np.full((80, 200, 3), 245, dtype=np.uint8)
        self.image[15:50, 20:35] = [10, 70, 220]
        self.image[20:40, 90:140] = [25, 25, 25]
        self.mask = np.ones(self.image.shape[:2], dtype=bool)

    def test_identical_native_pixels_pass(self):
        value = visual.image_metrics(self.image, self.image.copy(), self.mask, self.thresholds)
        self.assertTrue(value["passed"])
        self.assertEqual(value["meanAbsoluteRgbError"], 0)
        self.assertEqual(value["pixelAgreement"], 1)
        self.assertEqual(value["edgeF1"], 1)
        self.assertTrue(value["pixelsPassed"])
        self.assertTrue(value["geometryPassed"])

    def test_color_and_theme_mismatch_fails(self):
        dark = np.full_like(self.image, 32)
        value = visual.image_metrics(self.image, dark, self.mask, self.thresholds)
        self.assertFalse(value["passed"])
        self.assertGreater(value["meanAbsoluteRgbError"], 100)
        self.assertFalse(value["pixelsPassed"])

    def test_missing_real_controls_fails_despite_white_background(self):
        blank = np.full_like(self.image, 245)
        value = visual.image_metrics(self.image, blank, self.mask, self.thresholds)
        self.assertFalse(value["passed"])
        self.assertEqual(value["edgeF1"], 0)

    def test_displaced_geometry_does_not_pass(self):
        moved = np.roll(self.image, 7, axis=1)
        value = visual.image_metrics(self.image, moved, self.mask, self.thresholds)
        self.assertFalse(value["passed"])
        self.assertLess(value["edgeF1"], 0.9)

    def test_declared_dynamic_text_mask_only_excludes_that_content(self):
        changed = self.image.copy()
        changed[20:40, 90:140] = [245, 0, 160]
        self.mask[19:41, 89:141] = False
        self.assertTrue(visual.image_metrics(self.image, changed, self.mask, self.thresholds)["passed"])
        changed[15:50, 20:35] = 245
        self.assertFalse(visual.image_metrics(self.image, changed, self.mask, self.thresholds)["passed"])

    def test_dimensions_and_empty_masks_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "dimensions"):
            visual.image_metrics(self.image, self.image[:, :-1], self.mask, self.thresholds)
        with self.assertRaisesRegex(ValueError, "no measured pixels"):
            visual.image_metrics(self.image, self.image, np.zeros_like(self.mask), self.thresholds)
        with self.assertRaises(ValueError):
            visual.checked_rectangle([0, 0, 201, 80], 200, 80)
        with self.assertRaises(ValueError):
            visual.checked_rectangle([False, 0, 200, 80], 200, 80)

    def test_reference_hash_mismatch_fails(self):
        with tempfile.TemporaryDirectory(prefix="explorer-visual-hash-") as folder:
            path = Path(folder) / "source.png"
            Image.fromarray(self.image).save(path)
            visual.verify_file(path, visual.sha256(path))
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                visual.verify_file(path, "0" * 64)

    def test_non_native_or_non_isolated_capture_fails(self):
        capture = {"headless": True, "privateDesktop": True, "inputDesktopUnchanged": True,
                   "printWindowSucceeded": True, "renderer": "native-PrintWindow-WIC",
                   "visibleInputDesktopWindows": False, "width": 200, "height": 80,
                   "windowDpi": 96, "layoutDpi": 96, "unpaintedFraction": 0,
                   "uniqueColors": 20, "visibleChildren": 2}
        # Use gradients so a real unblanked image has more than twelve colors.
        gradient = self.image.copy()
        gradient[:, :80, 0] = np.arange(80, dtype=np.uint8)
        image = Image.fromarray(gradient)
        self.assertEqual(visual.capture_invariants(capture, image), [])
        for key, invalid in [("privateDesktop", False), ("inputDesktopUnchanged", False),
                             ("visibleInputDesktopWindows", True), ("renderer", "synthetic"),
                             ("windowDpi", 120)]:
            modified = {**capture, key: invalid}
            self.assertTrue(visual.capture_invariants(modified, image), key)
        # Valid-looking JSON cannot make a flat white PNG a real-widget capture.
        self.assertTrue(visual.capture_invariants(capture, Image.new("RGB", (200, 80), "white")))

    def test_reference_manifest_rejects_bad_schema_and_paths(self):
        manifest = json.loads((ROOT / "tests" / "visual" / "windows10-reference.json").read_text())
        visual.validate_manifest(manifest)
        for field, bad in [("file", "../outside.png"), ("sha256", "unverified"), ("kind", "Library")]:
            changed = copy.deepcopy(manifest)
            changed["sources"][0][field] = bad
            with self.assertRaises(ValueError):
                visual.validate_manifest(changed)
        changed = copy.deepcopy(manifest)
        changed["sources"].append(copy.deepcopy(changed["sources"][0]))
        with self.assertRaisesRegex(ValueError, "unique"):
            visual.validate_manifest(changed)
        changed = copy.deepcopy(manifest)
        changed["scenes"][0]["regions"][0]["actualOrigin"] = "guessed-caption"
        with self.assertRaisesRegex(ValueError, "measured"):
            visual.validate_manifest(changed)
        changed = copy.deepcopy(manifest)
        changed["scenes"][0]["regions"].append(copy.deepcopy(changed["scenes"][0]["regions"][0]))
        with self.assertRaisesRegex(ValueError, "unique"):
            visual.validate_manifest(changed)

    def test_colored_body_cannot_conceal_blank_expanded_ribbon(self):
        canvas = np.full((240, 500, 3), 255, dtype=np.uint8)
        # Rendered caption and file list provide abundant colors, while the
        # actual expanded command band is empty. Overall PNG diversity passes.
        canvas[10:25, :, 0] = np.arange(500, dtype=np.uint16) % 256
        canvas[160:230, :, 1] = np.arange(500, dtype=np.uint16) % 256
        capture = {"headless": True, "privateDesktop": True, "inputDesktopUnchanged": True,
                   "printWindowSucceeded": True, "renderer": "native-PrintWindow-WIC",
                   "visibleInputDesktopWindows": False, "width": 500, "height": 240,
                   "windowDpi": 96, "layoutDpi": 96, "unpaintedFraction": 0,
                   "uniqueColors": 256, "visibleChildren": 4,
                   "widgets": [{"class": "UIRibbonCommandBar", "visible": True,
                                "bounds": [1, 31, 499, 147]}]}
        failures = visual.capture_invariants(capture, Image.fromarray(canvas))
        self.assertEqual(failures, ["Expanded native Ribbon command band is blank or unpainted"])
        canvas[70:100, 20:150, 2] = np.arange(130, dtype=np.uint8)
        canvas[75:95, 155:170] = 0
        self.assertEqual(visual.capture_invariants(capture, Image.fromarray(canvas)), [])
        canvas[57:141, 3:497] = 255
        canvas[70:100, 20:30] = 0
        canvas[70:100, 40:50] = 0
        self.assertEqual(visual.capture_invariants(capture, Image.fromarray(canvas)), [])
        # Minimized native Ribbon has a tab strip and no expanded command band.
        capture["widgets"][0]["bounds"] = [1, 31, 499, 55]
        self.assertEqual(visual.capture_invariants(capture, Image.fromarray(canvas)), [])
        capture["widgets"] = []
        self.assertEqual(visual.capture_invariants(capture, Image.fromarray(canvas), require_ribbon=True),
                         ["Required native Ribbon command-bar HWND is absent"])
        for rgb in (canvas, canvas[::3, ::7], np.array([[0, 0, 0], [0, 1, 0], [0, 0, 1]], dtype=np.uint8)):
            self.assertEqual(visual.unique_rgb_colors(rgb), len(np.unique(rgb.reshape(-1, 3), axis=0)))

    def test_real_button_inventory_cannot_conceal_unpainted_collapse_caret(self):
        canvas = np.full((80, 200, 3), 255, dtype=np.uint8)
        canvas[40:70, :, 0] = np.arange(200, dtype=np.uint8)
        capture = {"headless": True, "privateDesktop": True, "inputDesktopUnchanged": True,
                   "printWindowSucceeded": True, "renderer": "native-PrintWindow-WIC",
                   "visibleInputDesktopWindows": False, "width": 200, "height": 80,
                   "windowDpi": 96, "layoutDpi": 96, "unpaintedFraction": 0,
                   "uniqueColors": 200, "visibleChildren": 1,
                   "widgets": [{"id": 139, "class": "Button", "visible": True,
                                "text": "Minimize the Ribbon", "bounds": [150, 5, 172, 29]}]}
        self.assertEqual(visual.capture_invariants(capture, Image.fromarray(canvas)),
                         ["Native Ribbon collapse caret is blank or unpainted"])
        # An outer frame alone cannot count as the missing content glyph.
        canvas[5:29, 150] = 0
        canvas[5:29, 171] = 0
        self.assertEqual(visual.capture_invariants(capture, Image.fromarray(canvas)),
                         ["Native Ribbon collapse caret is blank or unpainted"])
        canvas[15, 157:165] = 128
        self.assertEqual(visual.capture_invariants(capture, Image.fromarray(canvas)), [])

    def test_ribbon_inventory_origin_handles_caption_extended_client(self):
        # Generated images exercise only the comparison algorithm. The C++
        # executable independently proves actual private-desktop HWND capture.
        canvas = np.full((200, 500, 3), 255, dtype=np.uint8)
        canvas[31:111, 1:201] = self.image
        canvas[31:111, 1:81, 0] = np.arange(80, dtype=np.uint8)
        with tempfile.TemporaryDirectory(prefix="explorer-visual-anchor-") as folder:
            directory = Path(folder)
            path = directory / "generated.png"
            Image.fromarray(canvas).save(path)
            capture = {"headless": True, "privateDesktop": True, "inputDesktopUnchanged": True,
                       "printWindowSucceeded": True, "renderer": "native-PrintWindow-WIC",
                       "visibleInputDesktopWindows": False, "width": 500, "height": 200,
                       "windowDpi": 96, "layoutDpi": 96, "unpaintedFraction": 0,
                       "uniqueColors": 90, "visibleChildren": 1, "clientBounds": [1, 0, 499, 199],
                       "widgets": [{"class": "UIRibbonCommandBar", "visible": True, "bounds": [1, 31, 499, 147]}]}
            metadata = directory / "generated.json"
            metadata.write_text(json.dumps(capture))
            manifest = {"schema": 1, "sources": [{"id": "generated", "kind": "image", "file": path.name,
                        "sha256": visual.sha256(path), "publisher": "Generated algorithm fixture"}],
                        "scenes": [{"name": "Anchor", "reference": "generated", "dpiEvidence": "unit fixture",
                        "width": 500, "height": 200, "layoutDpi": 96,
                        "regions": [{"name": "anchor", "kind": "geometry", "reason": "Native Ribbon client can start at caption y=0",
                        "reference": [1, 31, 201, 111], "actual": [0, 0, 200, 80], "actualOrigin": "ribbon", "masks": []}]}],
                        "thresholds": self.thresholds, "limits": "Generated algorithm fixture only"}
            visual.validate_manifest(manifest)
            result = visual.compare_scene(manifest, "Anchor", directory, path, metadata, directory / "comparison")
            self.assertTrue(result["passed"])
            self.assertEqual(result["regions"][0]["actualCrop"], [1, 31, 201, 111])
            manifest["scenes"][0]["width"] = 501
            result = visual.compare_scene(manifest, "Anchor", directory, path, metadata, directory / "comparison")
            self.assertFalse(result["passed"])
            self.assertIn("dimensions differ", result["nativeCaptureFailures"][0])
            manifest["scenes"][0]["width"] = 500
            manifest["scenes"][0]["layoutDpi"] = 120
            result = visual.compare_scene(manifest, "Anchor", directory, path, metadata, directory / "comparison")
            self.assertFalse(result["passed"])
            self.assertIn("layout DPI differs", result["nativeCaptureFailures"][0])
            manifest["scenes"][0]["layoutDpi"] = 96
            capture["widgets"] = []
            metadata.write_text(json.dumps(capture))
            result = visual.compare_scene(manifest, "Anchor", directory, path, metadata, directory / "comparison")
            self.assertFalse(result["passed"])
            self.assertIn("no unique visible", result["regions"][0]["error"])
            capture["widgets"] = [{"class": "UIRibbonCommandBar", "visible": True, "bounds": [1, 31, 499, 147]}]
            widget_region = copy.deepcopy(manifest["scenes"][0]["regions"][0])
            widget_region.update({"name": "widget-anchor", "actualOrigin": "widget",
                "actualWidget": {"id": 105, "class": "Edit", "expectedHeight": 80}})
            manifest["scenes"][0]["regions"].append(widget_region)
            capture["widgets"].append({"id": 105, "class": "Edit", "visible": True, "bounds": [1, 31, 201, 111]})
            metadata.write_text(json.dumps(capture))
            visual.validate_manifest(manifest)
            result = visual.compare_scene(manifest, "Anchor", directory, path, metadata, directory / "comparison")
            self.assertTrue(result["passed"])
            self.assertEqual(result["regions"][1]["actualCrop"], [1, 31, 201, 111])
            widget_region["captureVariant"] = "modernQuickAccess"
            manifest["scenes"][0]["regions"][-1]["captureVariant"] = "modernQuickAccess"
            missing_variant = visual.compare_scene(manifest, "Anchor", directory, path, metadata, directory / "comparison")
            self.assertFalse(missing_variant["passed"])
            self.assertIn("capture variant is absent", missing_variant["regions"][-1]["error"])
            variant_metadata = directory / "quick-access.json"
            variant_capture = copy.deepcopy(capture)
            variant_capture["widgets"].append({"id": 104, "class": "Edit", "visible": False,
                "text": "::{679f85cb-0220-4080-b29b-5540cc05aab6}", "bounds": [1, 112, 450, 140]})
            variant_metadata.write_text(json.dumps(variant_capture))
            with_variant = visual.compare_scene(manifest, "Anchor", directory, path, metadata, directory / "comparison",
                {"modernQuickAccess": (path, variant_metadata)})
            self.assertTrue(with_variant["passed"])
            self.assertEqual(with_variant["regions"][-1]["actualCaptureVariant"], "modernQuickAccess")
            self.assertEqual(with_variant["regions"][-1]["nativeCaptureSha256"], visual.sha256(variant_metadata))
            variant_capture["widgets"][-1]["text"] = "D:\\Owned ordinary folder"
            variant_metadata.write_text(json.dumps(variant_capture))
            bad_variant = visual.compare_scene(manifest, "Anchor", directory, path, metadata, directory / "comparison",
                {"modernQuickAccess": (path, variant_metadata)})
            self.assertFalse(bad_variant["passed"])
            self.assertIn("actual native Quick Access", bad_variant["nativeCaptureFailures"][0])
            manifest["scenes"][0]["regions"][-1].pop("captureVariant")
            capture["widgets"][-1]["bounds"][-1] = 112
            metadata.write_text(json.dumps(capture))
            result = visual.compare_scene(manifest, "Anchor", directory, path, metadata, directory / "comparison")
            self.assertFalse(result["passed"])
            self.assertIn("widget height differs", result["regions"][1]["error"])
            capture["widgets"][-1]["bounds"][-1] = 111
            capture["widgets"].append(copy.deepcopy(capture["widgets"][-1]))
            metadata.write_text(json.dumps(capture))
            result = visual.compare_scene(manifest, "Anchor", directory, path, metadata, directory / "comparison")
            self.assertFalse(result["passed"])
            self.assertIn("no unique visible", result["regions"][1]["error"])
            capture["widgets"].pop()
            manifest["scenes"][0]["regions"].pop()
            capture["globalSettings"] = {"fileNameExtensions": {"readSucceeded": True, "matchesOs": True,
                "osValue": True, "ribbonValue": True, "registryValue": 0}}
            metadata.write_text(json.dumps(capture))
            for state in (True, False):
                alternative = copy.deepcopy(manifest["scenes"][0]["regions"][0])
                alternative["name"] = "native-setting-on" if state else "native-setting-off"
                alternative["condition"] = {"globalSetting": "fileNameExtensions", "equals": state}
                manifest["scenes"][0]["regions"].append(alternative)
            visual.validate_manifest(manifest)
            result = visual.compare_scene(manifest, "Anchor", directory, path, metadata, directory / "comparison")
            self.assertTrue(result["passed"])
            self.assertEqual(result["regionsCompared"], 2)
            self.assertIsNone(result["regions"][2]["passed"])
            self.assertFalse(result["regions"][2]["applicable"])
            # A claimed matchesOs=true cannot conceal contradictory readbacks.
            capture["globalSettings"]["fileNameExtensions"]["ribbonValue"] = False
            metadata.write_text(json.dumps(capture))
            result = visual.compare_scene(manifest, "Anchor", directory, path, metadata, directory / "comparison")
            self.assertFalse(result["passed"])
            self.assertIn("actual OS", result["regions"][1]["error"])
            capture["globalSettings"]["fileNameExtensions"] = {"readSucceeded": True, "matchesOs": True,
                "osValue": False, "ribbonValue": False, "registryValue": 1}
            metadata.write_text(json.dumps(capture))
            result = visual.compare_scene(manifest, "Anchor", directory, path, metadata, directory / "comparison")
            self.assertTrue(result["passed"])
            self.assertFalse(result["regions"][1]["applicable"])
            self.assertTrue(result["regions"][2]["applicable"])


if __name__ == "__main__":
    unittest.main()
