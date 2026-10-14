#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The packaging must describe the files in this tree, not stale copies."""

import hashlib
import re
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SEP = Path(__file__).resolve().parents[1]
ARCH = SEP / "packaging" / "arch"
FEDORA = SEP / "packaging" / "fedora"


def bash_array(text, name):
    match = re.search(r"^%s=\((.*?)\)" % re.escape(name), text, re.M | re.S)
    if match is None:
        raise AssertionError("no %s array" % name)
    return re.findall(r"'([^']*)'|\"([^\"]*)\"|(\S+)", match.group(1))


def words(text, name):
    return ["".join(parts) for parts in bash_array(text, name)]


class PkgbuildTest(unittest.TestCase):
    def check(self, directory):
        text = (directory / "PKGBUILD").read_text()
        sources = words(text, "source")
        sums = words(text, "sha256sums")
        self.assertEqual(len(sources), len(sums))
        pkgver = re.search(r"^pkgver=(\S+)$", text, re.M).group(1)
        for source, expected in zip(sources, sums):
            if "://" in source:
                continue
            name = source.replace("$pkgver", pkgver)
            path = directory / name
            self.assertTrue(path.is_file(), "%s is not a file" % path)
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            self.assertEqual(
                digest, expected, "%s changed: run updpkgsums in %s" % (name, directory)
            )

    def test_aurora_sep_sums_match_tree(self):
        self.check(ARCH / "aurora-sep")


class FedoraSpecTest(unittest.TestCase):
    def test_sources_are_in_tree(self):
        text = (FEDORA / "aurora-sep.spec").read_text()
        sources = re.findall(r"^Source\d+:\s+(\S+)$", text, re.M)
        self.assertTrue(sources)
        for source in sources:
            self.assertTrue((SEP / source).is_file(), source)

    def test_build_script_builds_only_aurora_sep(self):
        text = (FEDORA / "build.sh").read_text()
        self.assertIn("aurora-sep.spec", text)
        self.assertNotIn("dnf -y download --source libfprint", text)


class LoaderOrderingTest(unittest.TestCase):
    def preflight(self, values=None):
        # Execute the real loader up to insertion, without executing modprobe,
        # touching real SEP state, or taking a keyring session on the host.
        source = (SEP / "load-driver").read_text()
        prelude = source.split("\nif ! modprobe --first-time apple_sep ", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            module = Path(directory) / "apple_sep"
            if values is not None:
                (module / "parameters").mkdir(parents=True)
                for parameter, value in values.items():
                    (module / "parameters" / parameter).write_text(value + "\n")
            script = prelude.replace("/sys/module/apple_sep", str(module)) + "\nprintf 'insertion reached\\n'\n"
            environment = dict(os.environ, AURORA_SEP_SESSION="1")
            return subprocess.run(["sh", "-c", script], env=environment, text=True, capture_output=True)

    def test_first_load_passes_preflight(self):
        result = self.preflight()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("insertion reached", result.stdout)

    def test_any_existing_attachment_refuses_without_reload(self):
        # Even fabricated matching parameter files cannot prove compatibility:
        # the public Rust module exposes no such sysfs parameter interface.
        for values in ({"xart_writes": "1", "provision_keybag": "1"},
                       {"xart_writes": "1", "provision_keybag": "0"}, {}):
            with self.subTest(values=values):
                result = self.preflight(values)
                self.assertEqual(result.returncode, 1)
                self.assertNotIn("insertion reached", result.stdout)
                self.assertIn("reboot", result.stderr)
                self.assertIn("cannot be verified", result.stderr)
                self.assertIn("do not unload or reload", result.stderr)
        self.assertNotIn("/sys/module/apple_sep/parameters", (SEP / "load-driver").read_text())

    def test_actual_insertion_failure_refuses_without_probing_or_retry(self):
        source = (SEP / "load-driver").read_text()
        # Run the actual modprobe failure branch with a recording shell command;
        # stop before the later key/device probes. No real module is inserted.
        prelude = source.split("\nwork=$(mktemp", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            module = Path(directory) / "absent-module"
            script = ("modprobe() { printf 'attempt:%s\\n' \"$*\"; return 1; }\n"
                      + prelude.replace("/sys/module/apple_sep", str(module))
                      + "\nprintf 'unsafe continuation\\n'\n")
            result = subprocess.run(["sh", "-c", script], env=dict(os.environ, AURORA_SEP_SESSION="1"),
                                    text=True, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("attempt:--first-time apple_sep xart_writes=1 provision_keybag=1", result.stdout)
        self.assertEqual(result.stdout.count("attempt:"), 1)
        self.assertNotIn("unsafe continuation", result.stdout)
        self.assertIn("preserve existing state", result.stderr)
        self.assertIn("check the kernel log", result.stderr)
        self.assertNotIn("race", result.stderr)

    def test_alias_blacklist_is_packaged_without_blocking_explicit_load(self):
        effective = [line for line in (SEP / "aurora-sep-firmware-order.conf").read_text().splitlines()
                     if line and not line.startswith("#")]
        self.assertEqual(effective, ["blacklist apple_sep"])
        for recipe in (ARCH / "aurora-sep/PKGBUILD", FEDORA / "aurora-sep.spec"):
            source = recipe.read_text()
            self.assertIn("/lib/modprobe.d/aurora-sep-firmware-order.conf", source)
            self.assertNotIn("omarchy-vendor-firmware.service", source)
        self.assertIn("modprobe --first-time apple_sep xart_writes=1 provision_keybag=1", (SEP / "load-driver").read_text())


if __name__ == "__main__":
    unittest.main()
