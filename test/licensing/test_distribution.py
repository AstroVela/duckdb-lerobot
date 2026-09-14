"""Regression checks for release provenance, GPL rejection, and relinking inputs."""

import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import package_release as package
import rebuild_release as rebuild


class DistributionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.report = {
            "libraries": [
                {"name": name, "license": "LGPL version 2.1 or later", "configuration": "--disable-autodetect"}
                for name in sorted(package.FFMPEG_LIBRARIES)
            ]
        }

    def test_lgpl_report(self):
        package.validate_report(self.report)

    def test_every_library_must_be_lgpl(self):
        for index in range(4):
            for license in ("GPL version 2 or later", "GPL version 3 or later", "unknown"):
                report = copy.deepcopy(self.report)
                report["libraries"][index]["license"] = license
                with self.subTest(index=index, license=license), self.assertRaises(ValueError):
                    package.validate_report(report)

    def test_config_checked_independently_of_license_label(self):
        for flag in ("--enable-gpl", "--enable-nonfree", "--enable-libx264", "--enable-libx265", ""):
            report = copy.deepcopy(self.report)
            report["libraries"][0]["configuration"] = flag
            with self.subTest(flag=flag), self.assertRaises(ValueError):
                package.validate_report(report)

    def test_missing_or_duplicate_library(self):
        for libraries in (self.report["libraries"][:3], [self.report["libraries"][0]] * 4):
            with self.assertRaises(ValueError):
                package.validate_report({"libraries": libraries})

    def test_path_traversal_rejected(self):
        for path in ("../secret", "/etc/passwd", "C:/secret", "foo\\bar", "", "x/../../secret"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                package.checked_relative(path)
            with self.subTest(path=path), self.assertRaises(ValueError):
                rebuild.bundle_path(self.root, path)

    def test_missing_source_fails(self):
        with self.assertRaisesRegex(ValueError, "Missing source resources"):
            package.source_archives({"packages": []}, "ffmpeg", self.root, {})

    def test_default_host_triplet_can_differ_from_target(self):
        packages = [{"name": "vcpkg-cmake-get-vars", "triplet": "x64-linux"}]
        self.assertEqual(package.host_triplet({}, packages), "x64-linux")
        self.assertEqual(package.host_triplet({"VCPKG_HOST_TRIPLET": "arm64-osx"}, packages), "arm64-osx")
        with self.assertRaises(ValueError):
            package.host_triplet({}, [])

    def test_source_selected_by_hash(self):
        data = b"the exact source archive"
        path = self.root / "source.tar.gz"
        path.write_bytes(data)
        hash = hashlib.sha512(data).hexdigest()
        spdx = {
            "packages": [
                {
                    "SPDXID": "SPDXRef-resource-0",
                    "downloadLocation": "upstream",
                    "checksums": [{"algorithm": "SHA512", "checksumValue": hash}],
                }
            ]
        }
        self.assertEqual(package.source_archives(spdx, "ffmpeg", self.root, {hash: path}), [path])
        with self.assertRaisesRegex(ValueError, "Missing source archive"):
            package.source_archives(spdx, "ffmpeg", self.root, {})

    def test_checksum_tampering(self):
        with self.assertRaisesRegex(ValueError, "Checksum mismatch"):
            package.verify_checksum(b"changed", [{"algorithm": "SHA256", "checksumValue": "0" * 64}], "lib.a")

    def test_only_verified_static_runtime_dependencies(self):
        owners = {self.root / f"{name}.a": "ffmpeg" for name in package.FFMPEG_LIBRARIES}
        owners.update({self.root / f"lib{name}.a": name for name in ("aom", "dav1d", "zlib")})
        links = self.root / "links.txt"
        links.write_text("\n".join(map(str, owners)) + "\n-pthread\nm\n")
        package.validate_links(links, owners)
        with links.open("a") as stream:
            stream.write("/usr/lib/libdl.a\n/System/Library/Frameworks/CoreGraphics.framework\nmfplat.lib\n")
        package.validate_links(links, owners)
        with self.assertRaisesRegex(ValueError, "Unverified link"):
            package.validate_links(links, {})
        with links.open("a") as stream:
            stream.write("/external/libunreviewed.so\n")
        with self.assertRaisesRegex(ValueError, "Unverified link"):
            package.validate_links(links, owners)
        owners[self.root / "libx265.a"] = "x265"
        links.write_text("\n".join(map(str, owners)))
        with self.assertRaisesRegex(ValueError, "Unreviewed linked runtime"):
            package.validate_links(links, owners)

    def test_snapshot_excludes_generated_ci_environment(self):
        def git(*args):
            subprocess.run(["git", "-C", str(self.root), *args], check=True, stdout=subprocess.DEVNULL)

        git("init", "--quiet")
        (self.root / "source.cpp").write_text("source")
        git("add", "source.cpp")
        git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "--quiet", "-m", "fixture")
        (self.root / "docker_env.txt").write_text("BUILD_CREDENTIAL=must-not-be-packaged")
        files, revisions = {}, {}
        package.snapshot(self.root, "source", files, revisions)
        self.assertEqual(set(files), {"source/source.cpp"})
        self.assertEqual(len(revisions["source"]), 40)

    def test_bundle_detects_modified_sources(self):
        source = self.root / "source.cpp"
        source.write_bytes(b"source")
        manifest = {"schema_version": 1, "files": {"source.cpp": package.digest(source)}}
        (self.root / "distribution-manifest.json").write_text(json.dumps(manifest))
        rebuild.verify_bundle(self.root)
        source.write_bytes(b"modified")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            rebuild.verify_bundle(self.root)

    @unittest.skipIf(os.name == "nt", "Windows symlink privileges vary")
    def test_bundle_rejects_external_symlink(self):
        (self.root / "escape").symlink_to(self.root.parent)
        with self.assertRaisesRegex(ValueError, "escapes"):
            rebuild.bundle_path(self.root, "escape")

    def test_user_patch_is_added_to_exact_recipe(self):
        recipe = self.root / "ports/triplet/ffmpeg"
        recipe.mkdir(parents=True)
        (recipe / "portfile.cmake").write_text(
            "vcpkg_from_github(\n    REPO ffmpeg/ffmpeg\n    PATCHES\n        original.patch\n)\n"
        )
        patch = self.root / "custom.patch"
        patch.write_text("user's FFmpeg modification")
        manifest = {"packages": [{"name": "ffmpeg", "triplet": "triplet"}]}
        rebuild.prepare_ports(self.root, self.root / "modified", manifest, patch)
        result = (self.root / "modified/ffmpeg/portfile.cmake").read_text()
        self.assertIn("PATCHES\n        lerobot-user.patch\n        original.patch", result)
        self.assertEqual((self.root / "modified/ffmpeg/lerobot-user.patch").read_text(), patch.read_text())


if __name__ == "__main__":
    unittest.main()
