#!/usr/bin/env python3
"""Check that a GoogleTest XML report contains at least one passing test."""

import argparse
import sys
from pathlib import Path
from xml.etree import ElementTree


def check_report(path):
    root = ElementTree.parse(path).getroot()
    if root.tag != "testsuites":
        raise ValueError("expected a GoogleTest testsuites report")

    cases = list(root.iter("testcase"))
    ran = [case for case in cases if case.get("status") == "run"]
    disabled = [case for case in cases if case.get("status") == "notrun"]
    skipped = sum(case.get("result") == "skipped" or case.find("skipped") is not None for case in ran)
    if (int(root.attrib["tests"]) != len(cases) or len(cases) != len(ran) + len(disabled)
            or int(root.attrib["disabled"]) != len(disabled)):
        raise ValueError("declared tests do not match the ran and disabled cases")
    if int(root.attrib["failures"]) or int(root.attrib["errors"]):
        raise ValueError("the suite reported failures")
    if len(ran) == skipped:
        raise ValueError(f"no passing tests ({len(ran)} ran, {skipped} skipped, {len(disabled)} disabled)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        check_report(args.report)
    except (OSError, KeyError, ValueError, ElementTree.ParseError) as error:
        print(f"{args.report}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
