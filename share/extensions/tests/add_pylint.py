#!/usr/bin/env python3
# coding=utf-8
#
# Copyright (C) 2019 Martin Owens
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA.
#
"""
Add pylint scores to html coverage report html.

This would be done deeper in coverage (as a plugin) but that functionality isn't available.
"""

import os
import re
import sys
from typing import Tuple
from pylint import lint
from pylint.reporters.text import TextReporter

DIR = os.path.dirname(__file__)
REX = re.compile(r"<tr\ class=\"region\"\>.+?\">([^<]+\.py).+?\<\/tr\>")

ARGS = ["--rcfile=" + os.path.join(DIR, "..", ".pylintrc")]
stdout = sys.stdout


class WritableObject(object):
    """dummy output stream for pylint"""

    def __init__(self):
        self.content = []

    def write(self, line):
        "dummy write"
        self.content.append(line)

    def read(self):
        "dummy read"
        return self.content


def run_pylint(fname) -> Tuple[int, float]:
    "run pylint on the given file"
    pylint_output = WritableObject()
    # Pipe lint errors to devnull
    temp, sys.stderr = sys.stderr, open(os.devnull, "w", encoding="utf-8")
    try:
        lint.Run([fname] + ARGS, reporter=TextReporter(pylint_output), exit=False)
    except Exception:  # pylint: disable=broad-except
        return None
    sys.stderr = temp
    num_statements = 0
    score = -11.0
    for output in pylint_output.read():
        statements = re.findall(r"^(\d+) statements analysed.$", output)
        for st in statements:
            num_statements = int(st)
        rates = re.findall(r"rated at (\-?[\d\.]+)", output)
        for rate in rates:
            score = float(rate)
    return num_statements, score


def add_lint(fname):
    """
    Parse index.html and append in the needed pylint score for this file.
    """
    # Read in index file and strip out html whitespace (for easier rex'ing)
    with open(fname, "r", encoding="utf-8") as fhl:
        html = re.sub(r"\>\s+\<", "><", fhl.read())

    # Keep a tab on how much we've inserted into the html
    adjust = 0
    total_score = 0
    total_statements = 0
    for match in REX.finditer(html):
        statements, score = run_pylint(match.groups()[0])
        total_score += statements * score
        total_statements += statements
        (start, end) = match.span()
        start += adjust
        end += adjust
        old_content = html[start:end]
        new_content = old_content[:-5] + f"<td>{score}</td></tr>"
        html = html[:start] + new_content + html[end:]
        adjust += len(new_content) - len(old_content)

    total = 0
    if total_statements > 0:
        total = total_score / total_statements
    html = html.replace("coverage</th>", "coverage</th><th>pylint</th>")
    html = html.replace("</tr></tfoot>", f"<td>{total:.2f}</td></tr></tfoot>")

    with open(fname, "w") as fhl:
        fhl.write(html)


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[-1].endswith(".html"):
        for filename in sys.argv[1:]:
            if os.path.isfile(filename):
                add_lint(filename)
    else:
        for my_py_file in sys.argv[1:]:
            score = run_pylint(my_py_file)[0]
            print(f"{score},{my_py_file}")
