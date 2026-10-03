#!/usr/bin/env python

import argparse
from lxml import etree


def argparser():
    """Create an argument parser"""
    parser = argparse.ArgumentParser(
        description="Generate an SVG visibility test image"
    )
    parser.add_argument(
        "--output",
        "-o",
        type=argparse.FileType("w"),
        default="visibility_testcase.svg",
        help="Output file",
    )
    # Parse the command-line arguments
    return parser.parse_args()


def create_svg(x=100, y=100):
    """Create a new SVG document"""
    svg = etree.Element(
        "svg",
        {
            "xmlns": "http://www.w3.org/2000/svg",
            "width": "100%",
            "height": "100%",
            "viewBox": f"0 0 {x} {y}",
        },
    )
    # Append generator comment
    svg.append(etree.Comment(" Generated with visibility_testcase_generator.py "))
    return svg


def add_css(document, css):
    """Add CSS definition to document"""
    etree.SubElement(document, "style", {"type": "text/css"}).text = etree.CDATA(css)


def get_size(descendants):
    """Calculate stepping size"""
    stepping = 1
    # multiply list length of every other step to get stepping
    for i in range(0, len(descendants), 2):
        stepping *= len(*descendants[i].values())
    return stepping


def add_children(base, *descendants):
    """Add child elements to base including nested children"""
    # unpack elements
    (name, elements), *rest = descendants[0].items()
    # get stepping width/height (using complex number)
    stepping = get_size(descendants[2:])
    if len(descendants) % 2 == 0:
        stepping *= 1j
    # get basename (include ancestors)
    basename = base.get("id", "") + name
    index = 0
    # for each child element
    for tag, attr, *extra in elements:
        href = 0
        # set identifier from basename
        attr |= {"id": f"{basename}{index}"}
        # for clones set href from extra args
        if tag == "use":
            if extra:
                href = int(extra[0].get("href", 0))
            attr |= {"href": f"#{basename}{href}"}
        # calculate stepping translation (relative to reference if any)
        pos = (index - href) * stepping
        attr |= {"transform": f"translate({pos.real}, {pos.imag})"}
        # create child element
        e = etree.SubElement(base, tag, attr)
        # on groups add descendants with recursion
        if tag == "g" and len(descendants) > 1:
            add_children(e, *descendants[1:])
        index += 1


def generate_testcase(descendants, css):
    """Generate SVG testcase with nested children"""
    svg = create_svg(x=get_size(descendants[0:]), y=get_size(descendants[1:]))
    add_css(svg, css)

    # add nested children
    add_children(svg, *descendants)

    # Write the SVG document to the output file
    with argparser().output as f:
        f.write(
            etree.tostring(
                svg, pretty_print=True, xml_declaration=True, encoding="utf-8"
            ).decode()
        )


css = """
    rect { fill: darkblue; }
    .visibility { visibility: visible; }
    .novisibility { visibility: hidden; }
    .inheritvisibility { visibility: inherit; }
    .display { display: inline; }
    .nodisplay { display: none; }
    .opacity { opacity: 1; }
    .noopacity { opacity: 0; }
  """


# Define rectangle element
rect = (("rect", {"width": "1", "height": "1"}),)

# Define groups with different visibility settings
visibility = (
    ("g", {}),
    ("g", {"style": "visibility: visible;"}),
    ("g", {"style": "visibility: hidden;"}),
    ("g", {"style": "visibility: inherit;"}),
    ("g", {"visibility": "visible"}),
    ("g", {"visibility": "hidden"}),
    ("g", {"visibility": "inherit"}),
    ("g", {"style": "visibility: visible;", "visibility": "hidden"}),
    ("g", {"style": "visibility: hidden;", "visibility": "visible"}),
    ("g", {"style": "visibility: inherit;", "visibility": "visible"}),
    ("g", {"style": "visibility: visible;", "visibility": "inherit"}),
    ("g", {"style": "visibility: hidden;", "visibility": "inherit"}),
    ("g", {"style": "visibility: inherit;", "visibility": "hidden"}),
    ("g", {"visibility": "hidden", "class": "visibility"}),
    ("g", {"class": "novisibility", "visibility": "visible"}),
    ("g", {"class": "inheritvisibility", "visibility": "visible"}),
    ("use", {"visibility": "visible"}, {"href": 15}),
    ("use", {"class": "novisibility"}, {"href": 15}),
    ("use", {"visibility": "inherit"}, {"href": 15}),
    ("use", {"class": "visibility"}, {"href": 15}),
)

# Define groups with different display settings
display = (
    ("g", {}),
    ("g", {"style": "display: none;"}),
    ("g", {"style": "display: inline;"}),
    ("g", {"display": "none"}),
    ("g", {"display": "inline"}),
    ("g", {"style": "display: none;", "display": "inline"}),
    ("g", {"style": "display: inline;", "display": "none"}),
    ("use", {"class": "nodisplay"}, {"href": 0}),
    ("use", {"display": "none", "class": "display"}, {"href": 0}),
)

# Define groups with different opacity settings
opacity = (
    ("g", {}),
    ("g", {"style": "opacity:0;"}),
    ("g", {"style": "opacity:1;"}),
    ("g", {"opacity": "0"}),
    ("g", {"opacity": "1"}),
    ("g", {"style": "opacity:0;", "opacity": "1"}),
    ("g", {"style": "opacity:1;", "opacity": "0"}),
    ("use", {"class": "noopacity"}, {"href": 0}),
    ("use", {"opacity": "0", "class": "opacity"}, {"href": 0}),
)

descendants = (
    {"O": opacity},
    {"D": display},
    {"VA": visibility},
    {"VB": visibility},
    {"rect": rect},
)


if __name__ == "__main__":
    generate_testcase(descendants, css)
