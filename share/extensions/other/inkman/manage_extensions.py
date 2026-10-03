#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2018 Martin Owens <doctormo@gmail.com>

"""Inkscape Extensions Manager, Graphical User Interface"""

import sys
import warnings

from argparse import ArgumentParser

from inkman.backfoot import attempt_to_recover

warnings.filterwarnings("ignore")


def run(args):
    # Late imports to catch import errors.
    from inkman.targets import TARGETS
    from inkman.gui import ManagerApp
    from inkman.utils import get_inkscape_version

    arg_parser = ArgumentParser(description=__doc__)
    arg_parser.add_argument("input_file", nargs="?")
    arg_parser.add_argument(
        "--target",
        default=TARGETS[0].category,
        choices=[t.category for t in TARGETS],
    )
    arg_parser.add_argument("--for-version", default=None)
    options = arg_parser.parse_args(args)
    version = options.for_version or get_inkscape_version()
    ManagerApp(
        version=version,
        target=[t for t in TARGETS if t.category == options.target][0],
    ).run_wrapped()


def recovery_run(args):
    # Register an exception hook, rather than using a try/except block, because exceptions in GTK
    # callbacks won't necessarily bubble up to a try/except.
    sys.excepthook = attempt_to_recover
    run(args)


if __name__ == "__main__":
    recovery_run(sys.argv[1:])
