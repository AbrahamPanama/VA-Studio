# SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
# A simple toy to test the parser
# Copyright 2008 Aaron Spike <aaron@ekips.org>

import sys
sys.path.append(os.path.join(os.path.dirname(__file__), "..", "py2geom"))
import py2geom

class TestSink(py2geom.SVGPathSink):
    def __init__(self):
        py2geom.SVGPathSink.__init__(self)
        self.data = []
    def __str__(self):
        return ' '.join(self.data)
    def moveTo(self, p):
        x,y = p
        self.data.append('M %s, %s' % (x,y))
    def lineTo(self, p):
        x,y = p
        self.data.append('L %s, %s' % (x,y))
    def curveTo(self, c0, c1, p): 
        c0x,c0y = c0
        c1x,c1y = c1
        x,y = p
        self.data.append('C %s, %s %s, %s %s, %s' % (c0x,c0y,c1x,c1y,x,y))
    def quadTo(self, c, p): 
        cx,cy = c
        x,y = p
        self.data.append('Q %s, %s %s, %s' % (cx,cy,x,y))
    def arcTo(self, rx, ry, angle, large_arc, sweep, p): 
        x,y = p
        self.data.append('A %s, %s %s %i %i %s, %s' % (rx,ry,angle,large_arc,sweep,x,y))
    def closePath(self): 
        self.data.append('Z')
    def flush(self): 
        pass

def test_path(description, in_path, out_path):
    s = TestSink()
    py2geom.parse_svg_path(in_path, s)
    if str(s) == out_path:
        print 'Success: %s' % description
        return True
    else:
        print 'Error: %s' % description
        print '    given    "%s"' % in_path
        print '    got      "%s"' % str(s)
        print '    expected "%s"' % out_path
        return False

def run_tests(tests):
    successes = 0
    failures = 0 
    for description, in_path, out_path in tests:
        if test_path(description, in_path, out_path):
            successes += 1
        else:
            failures += 1
    print '=' * 20
    print 'Tests: %s' % (successes + failures)
    print 'Good:  %s' % successes
    print 'Bad:   %s' % failures

if __name__=='__main__':
    tests = [
        ('lineto', 'M 10,10 L 4,4', 'M 10.0, 10.0 L 4.0, 4.0'),
        ('implicit lineto', 'M 10,10 L 4,4 5,5 6,6', 'M 10.0, 10.0 L 4.0, 4.0 L 5.0, 5.0 L 6.0, 6.0'),
        ('implicit lineto after moveto', 'M1.2.3.4.5.6.7', 'M 1.2, 0.3 L 0.4, 0.5 L 0.6, 0.7'),
        ('arcto', 'M 300 150 A 150, 120, 30, 1, 0, 200 100', 'M 300.0, 150.0 A 150.0, 120.0 30.0 1 0 200.0, 100.0'),
    ]
    run_tests(tests)
