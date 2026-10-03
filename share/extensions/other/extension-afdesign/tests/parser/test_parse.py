# Copyright (C) 2024 Jonathan Neuhauser <jonathan.neuhauser@outlook.com>
# SPDX-License-Identifier: GPL-2.0-or-later

from io import BytesIO
import json
import os

from inkaf.parser.extract import AFExtractor
from inkaf.parser.parse import AFParser
from inkaf.parser.json_encoder import EnhancedJSONEncoder
from inkaf.parser.consts import content_magic


# TODO make this a fixture
def get_file(rel_path):
    path_to_current_file = os.path.realpath(__file__)
    current_directory = os.path.split(path_to_current_file)[0]
    return os.path.join(current_directory, rel_path)


def extract():
    input = get_file("../files/afdesign/default.afdesign")
    with open(input, "rb") as stream:
        result = AFExtractor(stream)
        output = result.content.files["doc.dat"]
        return output


def test_extract():
    output = extract()
    # Check that we at least found the magic number
    assert output.startswith((content_magic).to_bytes(4, byteorder="little"))


def test_parse():
    input = extract()
    compare = get_file("../files/compare/default.json")

    result = AFParser(BytesIO(input))

    actual = json.dumps(result.parse(), cls=EnhancedJSONEncoder, indent=4)
    with open(compare, "r") as f:
        expected = f.read()
    try:
        assert actual == expected
    except AssertionError:
        with open("out.json", "w") as f:
            f.write(actual)
        assert actual == expected
