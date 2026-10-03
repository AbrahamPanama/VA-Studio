import io
import math

import inkex
from inkex.tester import TestCase, ComparisonMixin

from hpgl2_input import Hpgl2Input as HpglInput


class TestHpglFileBasic(ComparisonMixin, TestCase):
    """Run-through tests of HPGL"""

    effect_class = HpglInput
    compare_file = "io/test.hpgl"
    comparisons = [("--height=11.6929133858",)]  # in mm


class HPGLTest(TestCase):
    """Base class for HPGL tests"""

    def __init__(self, *args, **kw):
        self.effect_class = HpglInput
        super().__init__(*args, **kw)

    def run_to_layer(self, string, *args, bake=False, break_apart=False) -> inkex.Layer:
        """Runs the HPGL string, returns the first layer, baking transforms if requested"""
        doc = self.import_string(
            string,
            "--bake-transforms=" + str(bake),
            "--break-apart=" + str(break_apart),
            *args,
        )
        layers = doc.getroot().xpath("//svg:g[@inkscape:groupmode='layer']")
        return (layers or [None])[0][0]


class HPGLVectorTests(HPGLTest):
    """Test the vector group of HPGL"""

    def test_simple_lines(self):
        """Draw a triangle using PD (absolute drawing mode)"""
        # Example from Figure 20-1
        doc = self.run_to_layer("IN;SP1;PA0,0;PD2500,0,0,1500,0,0;")
        self.assertEqual(doc[0].path, inkex.Path("M 0 0 2500 0 0 1500 0 0"))

    def test_simple_bezier(self):
        """Examples for p for a simple cubic bezier curve"""
        doc = self.run_to_layer("IN;SP1; PA1,5PD;BZ2,8,4,2,5,5;")
        # Example from Figure 20-6
        self.assertEqual(doc[0].path, inkex.Path("M 1 5 C 2 8 4 2 5 5"))
        # Example from Figure 20-13
        doc = self.run_to_layer(
            "IN;SP1; PA1016,5080;PR;PD;BR0,3048,4572,0,3556,2032,-508,1016,2540,508,2540,-5080;"
        )
        result = inkex.Path(
            "M 1016,5080 c 0,3048,4572,0,3556,2032,-508,1016,2540,508,2540,-5080"
        )
        self.assertEqual(doc[0].path, result)
        # Same path in absolute notation
        doc = self.run_to_layer(
            "IN;SP1; PA1016,5080;PR;PD;BZ1016,8128,5588,5080,4572,7112,"
            "4064,8128,7112,7620,7112,2032;"
        )
        self.assertEqual(doc[0].path, result.to_absolute())

    def test_simple_arc(self):
        """Test a simple arc with positive and negative sense of rotation"""
        doc = self.run_to_layer(
            """IN;SP1; PA2000,0;PD;AA0,0,45,25;PU1050,1050;PD;
        AA0,0,-45,10;PU1000,0;PD;AA0,0,45""",
            break_apart=True,
        )
        # Example from Figure 20-9
        self.assertAlmostTuple(
            doc[0].path[1].args, [2000, 2000, 0, 0, 1, 1414.21, 1414.21]
        )
        self.assertAlmostTuple(doc[1].path[0].args, [1050, 1050])
        self.assertAlmostTuple(
            doc[1].path[1].args, [1484.92, 1484.92, 0.0, 0.0, 0.0, 1484.92, 0.0]
        )
        self.assertAlmostTuple(doc[2].path[0].args, [1000, 0])
        self.assertAlmostTuple(
            doc[2].path[1].args, [1000.0, 1000.0, 0.0, 0.0, 1.0, 707.107, 707.107]
        )

    def test_arc_flags(self):
        """Test some large arcs to check that the flags are correctly implemented"""
        doc = self.run_to_layer("IN;SP1; PA-2000,0;PD;AA0,0,270")
        self.assertAlmostTuple(doc[0].path[1].args, [2000, 2000, 0, 1, 1, 0, 2000])

        doc = self.run_to_layer("IN;SP1; PA-2000,0;PD;AA0,0,-270")
        self.assertAlmostTuple(doc[0].path[1].args, [2000, 2000, 0, 1, 0, 0, -2000])

        doc = self.run_to_layer("IN;SP1; PA2000,0;PD;AA0,0,-270")
        self.assertAlmostTuple(doc[0].path[1].args, [2000, 2000, 0, 1, 0, 0, 2000])

        doc = self.run_to_layer("IN;SP1; PA1414,1414;PD;AA0,0,-225")
        self.assertAlmostTuple(doc[0].path[1].args, [2000, 2000, 0, 1, 0, -2000, 0], 0)

        doc = self.run_to_layer("IN;SP1; PA1414,1414;PD;AA0,0,225")
        self.assertAlmostTuple(doc[0].path[1].args, [2000, 2000, 0, 1, 1, 0, -2000], 0)

    def test_arc_relative(self):
        """Test relative arcs"""
        # Example from Figure 20-10. IMO the rendering there is not correct (only looks
        # like this if the angle is 90 instead of 80 degrees)
        doc = self.run_to_layer("IN; SP1PA1500,1500PDAR0,2000,80,25;AR2000,0,80;")
        self.assertAlmostTuple(
            doc[0].path[1].args, [2000, 2000, 0, 0, 1, 1969.62, 1652.7]
        )
        self.assertAlmostTuple(
            doc[0].path[2].args, [2000, 2000, 0, 0, 1, 1652.7, -1969.62]
        )

    def test_arc_threepoint(self):
        """Test a real-world example with 3 point arcs"""
        doc = self.run_to_layer(
            """IN; SP1; PA1000,100; PD2500,100; 
            PU650,1150; PD1000,1150; PU650,450; PD1000,450; PU1000,100;
            PU1000,100;PD1000,1500,2500,1500;
            AT3200,800,2500,100;PU3200,900;PD
            AT3300,800,3200,700; PU3300,800;PD3500,800;"""
        )
        self.assertEqual(
            doc[0].path,
            inkex.Path(
                "M 1000 100 L 2500 100 M 650 1150 L 1000 1150 "
                "M 650 450 L 1000 450 M 1000 100 M 1000 100 "
                "L 1000 1500 L 2500 1500 "
                "A 700 700 0 0 0 3106.22 1150 "
                "A 700 700 0 0 0 3106.22 450 "
                "A 700 700 0 0 0 2500 100 M 3200 900 "
                "A 100 100 0 0 0 3286.6 850 "
                "A 100 100 0 0 0 3286.6 750 "
                "A 100 100 0 0 0 3200 700 M 3300 800 L 3500 800"
            ),
            str(doc[0].path),
        )

    def test_arc_threepoint_complex(self):
        """Test some more complex 3-point arcs"""

        inkex.PathElement.MAX_ARC_SUBDIVISIONS = 1
        sq5 = math.sqrt(5)

        def compare_arc(data, result):
            doc = self.run_to_layer(data)
            self.assertIsInstance(doc[0].path[1], inkex.paths.Arc)
            self.assertAlmostTuple(doc[0].path[1].args, result, 3)

        # CCW large arc
        compare_arc("IN;SP1; PA3,3;PD;AT0,2,4,0", [sq5, sq5, 0, 1, 1, 4, 0])

        # CCW large arc but angle jumps over 360
        compare_arc("IN;SP1; PA3,3;PD;AT0,2,4,2", [sq5, sq5, 0, 1, 1, 4, 2])

        # CCW small arc
        compare_arc("IN;SP1; PA3,3;PD;AT1,3,0,2", [sq5, sq5, 0, 0, 1, 0, 2])

        # CCW small arc but angle jumps over 360
        compare_arc("IN;SP1; PA3,-1;PD;AT4,0,3,3", [sq5, sq5, 0, 0, 1, 3, 3])

        # CW large arc
        compare_arc("IN;SP1; PA0,2;PD;AT3,3,4,0", [sq5, sq5, 0, 1, 0, 4, 0])

        # CW large arc but angle jumps over 360
        compare_arc("IN;SP1; PA1,-1;PD;AT3,3,3,-1", [sq5, sq5, 0, 1, 0, 3, -1])

        # CW small arc
        compare_arc("IN;SP1; PA0,2;PD;AT1,3,3,3", [sq5, sq5, 0, 0, 0, 3, 3])

        # CW small arc but angle jumps over 360
        compare_arc("IN;SP1; PA3,3;PD;AT4,0,3,-1", [sq5, sq5, 0, 0, 0, 3, -1])

        inkex.PathElement.MAX_ARC_SUBDIVISIONS = 4

    def test_arc_threepoint_on_line(self):
        """Test the case where all three points of a three-point-arc lie on a line"""
        doc = self.run_to_layer("IN;SP1; PA2,2;PD;AT3,3,4,4")
        self.assertIsInstance(doc[0].path[1], inkex.paths.Line)
        self.assertAlmostTuple(doc[0].path[1].args, [4, 4])
        doc = self.run_to_layer("IN;SP1; PD2,2;AT4,4,3,3")
        self.assertIsInstance(doc[0].path[2], inkex.paths.Move)
        self.assertAlmostTuple(doc[0].path[2].args, [3, 3])

    def test_drawing_penup(self):
        """Test that drawing arcs/beziers with pen up results in a move command
        to the end point"""

        def assert_move(data, result, command):
            doc = self.run_to_layer(data)
            # First command is move to (0,0), second command is lineto, third command
            # to be tested for
            for cmd in doc[0].path[2:]:
                self.assertIsInstance(cmd, command)
            self.assertAlmostTuple(doc[0].path[-1].args, result, 3)

        # Arc 3 Point always absolute
        assert_move("IN;SP1; PD3,3;PU;AT0,2,4,0", [4, 0], inkex.paths.Move)

        # Bezier
        assert_move("IN;SP1; PD3,3;PU;BZ2,8,4,2,5,5;", [5, 5], inkex.paths.Move)

        # Bezier relative
        assert_move("IN;SP1; PD3,3;PU;BR2,8,4,2,5,5;", [5, 5], inkex.paths.move)

        # Arc
        assert_move("IN;SP1; PD-2000,0;PU;AA0,0,270", [0, 2000], inkex.paths.Move)

        # Arc relative
        assert_move("IN;SP1; PD-20,10;PU;AR20,-10,270", [30, 10], inkex.paths.move)

    def test_polyline_encoded_b64(self):
        """Test a binary-encoded polyline"""

        def check_polyline(hex_data):
            pe_instr = bytes.fromhex(hex_data.replace(" ", ""))
            data = "IN;PE" + pe_instr.decode("latin-1")

            doc = self.run_to_layer(data)
            path = doc[0].path
            self.assertIsInstance(path[0], inkex.paths.Move)
            self.assertAlmostTuple(path[0].args, [5, 5])

            self.assertIsInstance(path[1], inkex.paths.line)
            self.assertAlmostTuple(path[1].args, [10.58, 0], 2)

            self.assertIsInstance(path[2], inkex.paths.line)
            self.assertAlmostTuple(path[2].args, [-5.58, 10.67], 2)

            self.assertIsInstance(path[3], inkex.paths.line)
            self.assertAlmostTuple(path[3].args, [-5, -10.67], 2)

        # Example from Figure 120
        check_polyline("3AC5 3EC6 3C 3D3FD33FD3 53E9BF 54D56BE9 40D36CE9 3B")

        # Insert a few garbage characters
        check_polyline("3AC5 3EC6 3C 3D3F 20 D33FD3 8A 53E9BF 54D56BE9 40D36CE9 3B")

        # Same command in 32 bit mode
        check_polyline(
            "37 3A65 3E66 3C 3D3F47603F4760 5353615F 544B604B5461 4047604C5461 3B"
        )

        # Switch to 32 bit halfway through
        check_polyline(
            "3AC5 3EC6 3C 3D3FD33FD3 37 5353615F 544B604B5461 4047604C5461 3B"
        )

        # To generate sample data for this unit test, this code can be used to encode
        # a single number.
        # import math
        # base = 32
        # n = input("Enter number: \n")

        # n = round(float(n) * 2**7)
        # if n < 0:
        #     n = 2 * abs(n) + 1
        # else:
        #     n = 2 * n
        # while (n >= base):
        #     print('{:x}'.format(63 + (n % base)))
        #     n = n // base
        # if base == 32:
        #     print('{:x}'.format(95 + n))
        # else:
        #     print('{:x}'.format(191 + n))

    def test_polygon_mode(self):
        """Test that polygon mode works"""
        # Example from Figure 21-21
        data = """IN; SP1; PA2000,2000; PM0;
        PD3000,2000, 3000, 3000; PD2000,3000,2000,2000; PM1;
        PD2080,2160, 2480,2160, 2480,2340, 2080,2340, 2080,2160; PM1;
        PD2080,2660, 2480,2660, 2480,2840, 2080,2840, 2080,2660; PM1;
        PD2920,2340, 2920,2660, 2720,2660; AA2720,2500,180;PD2920,2340;
        PM2;FP;SP3;EP;"""

        doc = self.run_to_layer(data)
        self.assertIsInstance(doc[0], inkex.PathElement)

        result = inkex.Path(
            """M 2000 2000 L 3000 2000 L 3000 3000 L 2000 3000 L 2000 2000 Z 
            M 2080 2160 L 2480 2160 L 2480 2340 L 2080 2340 2080 2160 Z 
            M 2080 2660 L 2480 2660 L 2480 2840 L 2080 2840 2080 2660 Z 
            M 2920 2340 L 2920 2660 L 2720 2660 A 160 160 0 0 1 2720 2340 L 2920 2340 Z
            """
        )
        self.assertEqual(doc[0].path, result)
        # Bottom path is filled with black, default fill-rule
        self.assertEqual(doc[0].style("fill"), inkex.Color("black"))
        self.assertEqual(doc[0].style("fill-rule"), "evenodd")
        # Top path is stroked green
        self.assertEqual(doc[1].path, result)
        self.assertEqual(doc[1].style("stroke"), inkex.Color("green"))

    def test_polygon_mode_penup(self):
        """Test polygon mode with penup / pendown and absolute / relative cmds"""

        data = """IN; SP1; PA45,35; PM0; PD 100,35;BZ 115,45,85,55,100,65;PU 100,85;
        AA85,85,180; PD; AA55,85,-180; PU; AT 30,75,40,65; PD; AT50,55,40,45; PM1;

        PR 25,10, 55,0; BR 15,10, -15,20, 0,30; PU 0, 20; AR -15,0,180; PD;AR-15,0,-180;
        PU; AT 50,95,60,85; PD; AT70,75,60,65; PR; PM2; FP; SP3; EP;
         """

        doc = self.run_to_layer(data)
        # Path (visually) verified with PloViewMini.
        self.assertEqual(
            doc[0].path,
            inkex.Path(
                """M 45 35 L 100 35 C 115 45 85 55 100 65 L 100 85 
                A 15 15 0 0 1 70 85 A 15 15 0 0 0 40 85 A 10 10 0 0 1 31.3398 80 
                A 10 10 0 0 1 31.3397 70 A 10 10 0 0 1 40 65 A 10 10 0 0 0 48.6603 60 
                A 10 10 0 0 0 48.6602 50 A 10 10 0 0 0 40 45 Z M 65 55 L 120 55 
                C 135 65 105 75 120 85 L 120 105 A 15 15 0 0 1 90 105 A 15 15 0 0 0 60 105 
                A 10 10 0 0 1 51.3398 100 A 10 10 0 0 1 51.3397 90 A 10 10 0 0 1 60 85 
                A 10 10 0 0 0 68.6602 80 A 10 10 0 0 0 68.6603 70 A 10 10 0 0 0 60 65 Z"""
            ),
            str(doc[0].path),
        )
        self.assertEqual(doc[0].style("fill"), inkex.Color("black"))
        self.assertEqual(
            doc[1].path,
            inkex.Path(
                """M 45 35 L 100 35 C 115 45 85 55 100 65 L 70 85 
                A 15 15 0 0 0 40 85 L 40 65 A 10 10 0 0 0 48.6603 60 
                A 10 10 0 0 0 48.6602 50 A 10 10 0 0 0 40 45 Z 
                M 65 55 L 120 55 C 135 65 105 75 120 85 L 90 105 
                A 15 15 0 0 0 60 105 L 60 85 A 10 10 0 0 0 68.6602 80 
                A 10 10 0 0 0 68.6603 70 A 10 10 0 0 0 60 65 Z"""
            ),
            str(doc[1].path),
        )
        self.assertEqual(doc[1].style("stroke"), inkex.Color("green"))

    def test_circle(self):
        """Test drawing circles"""
        # Check that we can draw circles correctly
        data = """IN; SP1; PA-170,20; CI75,45; PA30,20; CI75,30;"""
        doc = self.run_to_layer(data)
        result = inkex.Path(
            """M -245 20 a 75 75 0 1 1 150 0 a 75 75 0 1 1 -150 0 m 75 0
        M 30 20 m -75 0 a 75 75 0 1 1 150 0 a 75 75 0 1 1 -150 0 m 75 0"""
        )
        self.assertEqual(doc[0].path, result)

        # Check that the pen position is correctly updated
        data = """IN; SP1; PA-170,20; CI75,45; PR200,0; CI75,30;"""
        doc = self.run_to_layer(data)
        # the absolute version of the path must be unchanged
        self.assertEqual(doc[0].path.to_absolute(), result.to_absolute())

    def test_circle_pmode(self):
        """Test circles in polygon mode"""
        # This test is the most ambiguous unit test yet, a lot of different possible
        # outputs are correct. If this breaks, check visually.

        # Note on the fill rule: There should be a small inset, unfilled square in the
        # corner of the big rectangle. Not 100% about the fill of the circles -
        # they could be either filled or the intersection with the rectangle that's not
        # the union of the two circles could be empty, that depends on the sense of
        # orientation which is not entirely clear from the spec. In any case,
        # PloViewMini gets this wrong.

        data = """IN; SP1; PA 5, 5; PM0; PD; PR 100, 0, 0, 25, -100, 0, 0, -25 PU 30, 30 
        CI 20,5 PR 20, 0 CI 20, 5 PA 90, 20 PD PR 20, 0, 0, -20, -20, 0, 0, 20; PM2; FP1;
        SP4; EP;"""

        doc = self.run_to_layer(data)
        result = """M 5 5 L 105 5 L 105 30 L 5 30 L 5 5 L 35 35 Z 
        M 15 35 A 20 20 0 1 1 55 35 A 20 20 0 1 1 15 35 M 35 35 Z 
        M 55 35 M 35 35 A 20 20 0 1 1 75 35 A 20 20 0 1 1 35 35 M 55 35 Z 
        M 90 20 L 110 20 L 110 0 L 90 0 L 90 20 Z"""

        result2 = """M 5 5 L 105 5 L 105 30 L 5 30 L 5 5 Z 
        M 15 35 A 20 20 0 1 1 55 35 A 20 20 0 1 1 15 35 Z 
        M 55 35 M 35 35 A 20 20 0 1 1 75 35 A 20 20 0 1 1 35 35 Z 
        M 90 20 L 110 20 L 110 0 L 90 0 L 90 20 Z"""
        self.assertEqual(doc[0].path, inkex.Path(result))
        self.assertEqual(doc[0].style("fill"), inkex.Color("black"))
        self.assertEqual(doc[0].style("fill-rule"), "nonzero")
        self.assertEqual(doc[1].path, inkex.Path(result2))
        self.assertEqual(doc[1].style("fill"), None)
        self.assertEqual(doc[1].style("stroke"), inkex.Color("yellow"))

    def test_wedges(self):
        """Test wedges. Modified 21-19."""

        data = """IN;SP1; PA50,50;EW-1000,90,180;EW1000, 150,120;PR-60,110;WG-1000,270,60;SP3;EP;"""
        doc = self.run_to_layer(data)

        # First element is a 180 degree arc starting at 3*pi/2 and ending at pi/2
        self.assertAlmostTuple(doc[0].path[0].args, [50, -950])
        self.assertAlmostTuple(doc[0].path.to_absolute()[-3].args[-2:], [50, 1050])
        self.assertAlmostEqual(float(doc[0].get("sodipodi:start")), math.pi * 3 / 2)
        self.assertAlmostEqual(float(doc[0].get("sodipodi:end")), math.pi * 1 / 2)
        self.assertAlmostEqual(doc[0].get("sodipodi:arc-type", "slice"), "slice")
        self.assertAlmostEqual(float(doc[0].get("sodipodi:cx")), 50)
        self.assertAlmostEqual(float(doc[0].get("sodipodi:ry")), 1000)
        self.assertEqual(doc[0].style("fill"), None)
        self.assertEqual(doc[0].style("stroke"), inkex.Color("black"))

        # Second element starts at 150 degrees and ends at 3/2 pi
        self.assertAlmostEqual(float(doc[1].get("sodipodi:start")), math.pi * 150 / 180)
        self.assertAlmostEqual(float(doc[1].get("sodipodi:end")), math.pi * 3 / 2)
        self.assertEqual(doc[1].style("fill"), None)
        self.assertEqual(doc[1].style("stroke"), inkex.Color("black"))
        # Third element starts has a different center
        self.assertAlmostEqual(float(doc[2].get("sodipodi:cx")), -10)
        self.assertAlmostEqual(float(doc[2].get("sodipodi:cy")), 160)
        self.assertAlmostEqual(float(doc[2].get("sodipodi:end")), math.pi * 150 / 180)
        self.assertAlmostEqual(float(doc[2].get("sodipodi:start")), math.pi / 2)
        self.assertEqual(doc[2].style("fill"), inkex.Color("black"))
        self.assertEqual(doc[2].style("stroke"), None)

        # And then we reuse that wedge and fill it, the two paths should be identical
        self.assertEqual(doc[2].path.to_absolute(), doc[3].path.to_absolute())
        self.assertEqual(doc[3].style("fill"), None)
        self.assertEqual(doc[3].style("stroke"), inkex.Color("green"))

    def test_rectangles(self):
        """Test rectangles"""
        data = """IN;SP1;PA1200,400;FT;RR400,800;ER400,800;
        PR0,800;FT3,50;RA1600,2000;SP3;EP;"""
        doc = self.run_to_layer(data)
        bb0 = doc[0].bounding_box()
        # First rectangle
        self.assertAlmostEqual(bb0.left, 1200)
        self.assertAlmostEqual(bb0.top, 400)
        self.assertAlmostEqual(bb0.width, 400)
        self.assertAlmostEqual(bb0.height, 800)
        self.assertEqual(doc[0].style("fill"), inkex.Color("black"))
        self.assertEqual(doc[0].style("stroke"), None)

        # Second rectangle: pen position remains, relative, only stroke
        bb1 = doc[1].bounding_box()
        self.assertAlmostEqual(bb1.left, 1200)
        self.assertAlmostEqual(bb1.top, 400)
        self.assertAlmostEqual(bb1.width, 400)
        self.assertAlmostEqual(bb1.height, 800)
        self.assertEqual(doc[1].style("stroke"), inkex.Color("black"))
        self.assertEqual(doc[1].style("fill"), None)

        # Third rectangle: absolute, only fill
        bb2 = doc[2].bounding_box()
        self.assertAlmostEqual(bb2.left, 1200)
        self.assertAlmostEqual(bb2.top, 1200)
        self.assertAlmostEqual(bb2.right, 1600)
        self.assertAlmostEqual(bb2.bottom, 2000)
        self.assertEqual(doc[2].style("fill"), inkex.Color("black"))
        self.assertEqual(doc[2].style("stroke"), None)

        # Fourth rectangle reuses the polygon buffer
        self.assertEqual(
            inkex.Path(doc[2].get_path()).to_absolute(), doc[3].path.to_absolute()
        )
        self.assertEqual(doc[3].style("fill"), None)
        self.assertEqual(doc[3].style("stroke"), inkex.Color("green"))


class HpglConfigurationTests(HPGLTest):
    """Tests for the configurationg group"""

    def test_mirror(self):
        """Basic test for IP and SC command, based on Figure 19-6"""
        subr = """PU-15,-10;EA15,10;PA1,4;PD1,2,12,2 PU;"""
        data = f"""IN;
                SP1;
                PU1500,3600;ER1500,1500;
                IP1500,3600,3000,5100;SC-15,15,-10,10;{subr}
                SP2;IP3000,3600,1500,5100;{subr}
                SP3;IP1500,5100,3000,3600;{subr}
                SP4;IP3000,5100,1500,3600;{subr}"""

        group_matrices = [
            inkex.Transform(),
            inkex.Transform("matrix(50 0 0 75 2250 4350)"),
            inkex.Transform("matrix(-50 0 0 75 2250 4350)"),
            inkex.Transform("matrix(50 0 0 -75 2250 4350)"),
            inkex.Transform("matrix(-50 0 0 -75 2250 4350)"),
        ]

        doc = self.import_string(data, "--bake-transforms=False")
        document_mat = inkex.Transform("matrix(0.025 0 0 -0.025 0 254)")
        layer = doc.getroot().xpath("//svg:g[@inkscape:groupmode='layer']")[0]
        self.assertEqual(layer.transform, document_mat)
        for i in range(5):
            self.assertEqual(layer[i].transform, group_matrices[i])

        # Now different size
        doc = self.import_string(
            data, "--bake-transforms=False", "--width=4", "--height=5"
        )
        document_mat = inkex.Transform("matrix(0.025 0 0 -0.025 0 127)")
        layer = doc.getroot().xpath("//svg:g[@inkscape:groupmode='layer']")[0]
        self.assertEqual(layer.transform, document_mat)
        for i in range(5):
            self.assertEqual(layer[i].transform, group_matrices[i])

        # And different resolution
        doc = self.import_string(data, "--bake-transforms=False", "--resolution=2032.0")
        document_mat = inkex.Transform("matrix(0.0125 0 0 -0.0125 0 254)")
        layer = doc.getroot().xpath("//svg:g[@inkscape:groupmode='layer']")[0]
        self.assertEqual(layer.transform, document_mat)
        for i in range(5):
            self.assertEqual(layer[i].transform, group_matrices[i])

    # TODO rectangles as paths?
    def test_bake_transforms(self):
        """Test that transforms are baked correctly. Based on Figure 19-5"""

        data = """IN;IP500,500,5500,8000;SC0,10,0,15;SP1;PA0,0;PD10,0,10,15,0,15,0,0;PU;
                  IP5600,500;PA0,0;PD10,0,10,15,0,15,0,0;PU;"""

        doc = self.import_string(
            data, "--bake-transforms=False", "--width=12", "--height=8"
        )
        document_mat = inkex.Transform("matrix(0.025 0 0 -0.025 0 203.2)")
        layer = doc.getroot().xpath("//svg:g[@inkscape:groupmode='layer']")[0]
        group_transforms = [
            inkex.Transform("matrix(500 0 0 500 500 500)"),
            inkex.Transform("matrix(500 0 0 500 5600 500)"),
        ]
        self.assertEqual(layer.transform, document_mat)
        for i in range(2):
            self.assertEqual(layer[i].transform, group_transforms[i])

        doc = self.import_string(
            data, "--bake-transforms=True", "--width=12", "--height=8"
        )
        layer = doc.getroot().xpath("//svg:g[@inkscape:groupmode='layer']")[0]

        for i in range(2):
            self.assertEqual(layer[i].transform, inkex.Transform())

        self.assertEqual(
            layer[0][0].path,
            inkex.Path(
                "M 12.5 190.7 L 137.5 190.7 L 137.5 3.2 L 12.5 3.2 L 12.5 190.7"
            ),
        )
        self.assertEqual(
            layer[1][0].path,
            inkex.Path("M 140 190.7 L 265 190.7 L 265 3.2 L 140 3.2 L 140 190.7"),
        )

    def test_isotropic_scaling(self):
        data = """
        IN; SP1; 
        CO "draw the rectangles that will later be the 'PLC picture frames'";
        PA 10, 10; ER 1200, 1000;
        PA 1310, 10; ER 1200, 1000;
        PA 10, 1010; ER 1200, 1000;
        PA 1310, 1010; ER 1200, 1000;
        PA 2610, 10; ER 1200, 1000;
        PA 2610, 1010; ER 1200, 1000;

        SP3;

        IP10, 10,1210, 1010;
        SC0,10,0,10,1;
        PA0,0; PD0,10,10,10,10,0,0,0;

        IP1310, 10 ,2510, 1010;
        SC0,20,0,10,1;
        PA0,0; PD0,10,20,10,20,0,0,0;

        IP10, 1010,1210, 2010;
        SC0,10,0,10,1,0,0;
        PA0,0; PD0,10,10,10,10,0,0,0;

        IP1310, 1010,2510, 2010;
        SC0,20,0,10,1,0,0;
        PA0,0; PD0,10,20,10,20,0,0,0;

        IP2610, 10,3810, 1010;
        SC0,10,0,10,1,100,100;
        PA0,0; PD0,10,10,10,10,0,0,0;

        IP2610, 1010,3810, 2010;
        SC0,20,0,10,1,100,100;
        PA0,0; PD0,10,20,10,20,0,0,0;
        """
        doc = self.run_to_layer(data, bake=True).getparent()

        bottom = 254
        pitch = 2.5
        border = 0.25
        # Left / right space leftover

        # First rectangle is centered in its drawing area
        self.assertEqual(
            doc[1][0].bounding_box(),
            inkex.BoundingBox(
                (pitch + border, pitch + 10 * pitch + border),
                (bottom - border - 10 * pitch, bottom - border),
            ),
        )
        # Third rectangle "hangs left"
        self.assertEqual(
            doc[3][0].bounding_box(),
            inkex.BoundingBox(
                (border, 10 * pitch + border),
                (bottom - border - 20 * pitch, bottom - border - 10 * pitch),
            ),
        )
        # Fifth rectangle "hangs right" -> distance from left: 26 * pitch + 2 * pitch
        self.assertEqual(
            doc[5][0].bounding_box(),
            inkex.BoundingBox(
                (border + 28 * pitch, 38 * pitch + border),
                (bottom - border - 10 * pitch, bottom - border),
            ),
        )

        # Top / bottom space leftover
        self.assertEqual(
            doc[2][0].bounding_box(),
            inkex.BoundingBox(
                (pitch * 13 + border, 25 * pitch + border),
                (bottom - border - 8 * pitch, bottom - border - 2 * pitch),
            ),
        )
        # Rectangle "hangs bottom"
        self.assertEqual(
            doc[4][0].bounding_box(),
            inkex.BoundingBox(
                (pitch * 13 + border, 25 * pitch + border),
                (bottom - border - 16 * pitch, bottom - border - 10 * pitch),
            ),
        )
        # Rectangle "hangs top"
        self.assertEqual(
            doc[6][0].bounding_box(),
            inkex.BoundingBox(
                (pitch * 26 + border, 38 * pitch + border),
                (bottom - border - 20 * pitch, bottom - border - 14 * pitch),
            ),
        )

    def test_rotate(self):
        """Test rotating the coordinate system. Test based on Figure 19-15. Output
        differs from PloViewMini, but seems to be according to spec."""
        before = "SC0,10,0,15;PA0,0; PD0,15,10,15,10,0,0,0;PU;"
        after = (
            "PA0,0;PD0,15,10,15,10,0,0,0;PU;PA1,1;PD1,3;PU;SP3;PA1,1;PD3,1;PU;RO;SP1;"
        )
        data = f"""IN; SP1; 
            IP 100, 100,1100, 1600;{before}RO0;{after}
            IP 1200, 100, 2200, 1600;{before}RO 90;{after}
            IP 100, 1700, 1100, 3200;{before}RO180;{after}
            IP 1200, 1700, 2200, 3200;{before}RO270;{after}"""
        layer = self.run_to_layer(data, bake=True).getparent()
        # Query coordinates of the green paths
        # (pointing in positive x direction from (1,1) in user coordinates)
        # rotate 0 degrees
        self.assertEqual(layer[1][1].path, inkex.Path("M 5 249 L 10 249"))
        # rotate 90 degrees
        self.assertEqual(layer[3][1].path, inkex.Path("M 52.5 249 L 52.5 244"))
        # rotate 180 degrees
        self.assertEqual(layer[5][1].path, inkex.Path("M 25 176.5 L 20 176.5"))
        # rotate 270 degrees
        self.assertEqual(layer[7][1].path, inkex.Path("M 32.5 176.5 L 32.5 181.5"))

    def test_clip(self):
        """Test clipping without scaling"""
        data = """IN; SP1; 
            PA5000,3200;
            IW3000,1300,4500,3700
            PD2000,1700;PU3000,1300;
            PD4500,1300,4500,3700,3000,3700,3000,1300;PU;"""
        layer = self.run_to_layer(data, bake=True)

        self.assertEqual(
            layer.clip[0].bounding_box(), inkex.BoundingBox((75, 112.5), (161.5, 221.5))
        )
        layer = self.run_to_layer(data, bake=False)
        self.assertEqual(
            layer.clip[0].bounding_box(), inkex.BoundingBox((3000, 4500), (1300, 3700))
        )

    def test_clip_scaling(self):
        """Test clipping with scaling"""
        data = """IN; SP1; 
        SC 0,80,0,100;
        PA60,30;
        IW 20,10,50,30;
        PD10,6;PU20,10;
        PD20,30, 50,30, 50,10,20,10;PU;"""
        layer = self.run_to_layer(data, bake=True)
        self.assertEqual(
            layer.clip[0].bounding_box(), inkex.BoundingBox((50.8, 127), (177.8, 228.6))
        )
        layer = self.run_to_layer(data, bake=False)
        self.assertEqual(
            layer.clip[0].bounding_box(), inkex.BoundingBox((20, 50), (10, 30))
        )

    def test_clip_scaling_multiple(self):
        """Test clipping and moving the window with IP"""
        # The output of this test is slightly different than in PloViewMini,
        # but should be correct
        data = """IN; SP1; 
            IP 0,0,8128,10160
            SC 0,80,0,100;
            PA60,30;
            IW 20,10,50,30;
            PD10,6;PU20,10;
            PD20,30, 50,30, 50,10,20,10;PU;
            IP 1000,700,9128,10860SP2
            PA60,30;PD10,6;PU20,10;
            PD20,30, 50,30, 50,10,20,10;PU;
            SC 0,8,0,10
            IP 2000,1400,10128,11560SP3
            PA6,3;PD1,0.6;PU2,1;
            PD2,3, 5,3, 5,1,2,1;PU;
            IP 3000,2100,11128,12260SP4
            PA6,3;PD1,0.6;PU2,1;
            PD2,3, 5,3, 5,1,2,1;PU;"""
        layer = self.run_to_layer(data, bake=True).getparent()
        # First group: the clip is identical to the previous test
        self.assertEqual(
            layer[0].clip[0].bounding_box(),
            inkex.BoundingBox((50.8, 127), (177.8, 228.6)),
        )
        # Second group: the clip moves along with a IP command so that it has the
        # same coordinates in user units.
        self.assertEqual(
            layer[1].clip[0].bounding_box(),
            inkex.BoundingBox((75.8, 152), (160.3, 211.1)),
        )
        # Third group is after a SC command, so its clip has the same coordinates as
        # previously
        self.assertEqual(
            layer[2].clip[0].bounding_box(),
            inkex.BoundingBox((75.8, 152), (160.3, 211.1)),
        )
        # This is also not changed by another IP command
        self.assertEqual(
            layer[3].clip[0].bounding_box(),
            inkex.BoundingBox((75.8, 152), (160.3, 211.1)),
        )

        layer = self.run_to_layer(data, bake=False).getparent()
        self.assertEqual(
            layer[0].clip[0].bounding_box(), inkex.BoundingBox((20, 50), (10, 30))
        )
        self.assertEqual(
            layer[1].clip[0].bounding_box(), inkex.BoundingBox((20, 50), (10, 30))
        )
        self.assertEqual(
            layer[2].clip[0].bounding_box(),
            inkex.BoundingBox((1.01575, 4.01575), (0.311024, 2.31102)),
        )


class HpglStyleTests(HPGLTest):
    """Tests for the Line & Fill Attributes class"""

    def test_transparency(self):
        """Test transparency mode"""
        data = """IN; SP1; PD; PM0; PA 2000,0,1000,3000,0,0;PM2; FP; TR1;
        PU; PA 500, 1000; SP0; EA 1500,1000; RA 1500,1000;"""

        layer = self.run_to_layer(data)
        self.assertEqual(layer[1].style("stroke"), inkex.Color("white"))
        self.assertEqual(layer[1].style("stroke-opacity"), 0)
        self.assertEqual(layer[2].style("fill"), inkex.Color("white"))
        self.assertEqual(layer[2].style("fill-opacity"), 0)

        layer = self.run_to_layer(data.replace("TR1", "TR0"))
        self.assertEqual(layer[1].style("stroke"), inkex.Color("white"))
        self.assertEqual(layer[1].style("stroke-opacity"), 1)
        self.assertEqual(layer[2].style("fill"), inkex.Color("white"))
        self.assertEqual(layer[2].style("fill-opacity"), 1)

    def test_stroke_width_absolute(self):
        """Absolute stroke width"""
        data = """IN; SP1;PA 3500,2500;
            PW 1.5;PD 4500,2800,4500,1800,3500,1500,3500,2500;
            PW .8;PD 2300,2900,2300,1900,3500,1500;
            PW .5;PU 2300,2900;PD 3300, 3200, 4500, 2800;
            PW .25;PU 4500,1800;PD 3500,2100;"""
        layer = self.run_to_layer(data)
        self.assertEqual(float(layer[0].style("stroke-width")), 1.5)
        self.assertEqual(float(layer[1].style("stroke-width")), 0.8)
        self.assertEqual(float(layer[2].style("stroke-width")), 0.5)
        self.assertEqual(float(layer[3].style("stroke-width")), 0.25)

    def test_stroke_width_relative(self):
        """Relative stroke width"""
        data = """IN;
            IP0,0,2000,2000;SC0,10,0,10;
            SP1;WU1;PW0.003
            PA0,0;PD0,10,10,10,10,0,0,0;PU;
            PA5,5;CI3;
            IP2500,500,3500,1500;
            PA0,0;PD0,10,10,10,10,0,0,0;PU;
            PA5,5;CI3;
            PW;
            IP3500,500,4500,1500;
            PA5,5;CI3;"""
        layer = self.run_to_layer(data, bake=True).getparent()
        expected = layer[0][0].bounding_box().diagonal_length * 0.003
        self.assertAlmostEqual(expected, float(layer[0][0].style("stroke-width")))

        expected /= 2
        self.assertAlmostEqual(expected, float(layer[1][0].style("stroke-width")))

        expected /= 3
        self.assertAlmostEqual(expected, float(layer[2][0].style("stroke-width")))

    def test_line_type(self):
        """Line type"""
        data = """IN;LA1,4,2,4,3,5;IP0,0,4800,6400;SC0,150,0,200;SP1;
                PU0,0;PD60,0;
                LT0,5;PU0,5;PD;PR20,0;PR10,0;PR30,0;PA;
                LT1,5;PU0,10;PD;PR20,0;PR10,0;PR30,0;PA;
                LT2,5;PU0,15;PD;PR20,0;PR10,0;PR30,0;PA;
                LT3,5;PU0,20;PD;PR20,0;PR10,0;PR30,0;PA;
                LT4,5;PU0,25;PD;PR20,0;PR10,0;PR30,0;PA;
                LT5,5;PU0,30;PD;PR20,0;PR10,0;PR30,0;PA;
                LT6,5;PU0,35;PD;PR20,0;PR10,0;PR30,0;PA;
                LA1,2,2,4,3,5;
                LT2,5,1;PU 0,40;ER 20,20; 
                LT;PU 30,40;ER 20,20; 
                LT0,5,1;PU 60,40;ER 20,20; 
                UL2,0,15,0,15,0,15,40,15;
                LT2,5;PU0,75;PD;PR20,0;PR10,0;PR30,0;PA;
                UL2;
                LT2,5,0;PU0,80;PD;PR20,0;PR10,0;PR30,0;PA;
                UL2,0,15,0,15,0,15,40,15;
                LT2,5;PU0,85;PD;PR20,0;PR10,0;PR30,0;PA;
                UL;
                LT2,5,0;PU0,90;PD;PR20,0;PR10,0;PR30,0;PA;
                """
        layer = self.run_to_layer(data, bake=True)
        # First line is solid
        self.assertEqual(layer[0].style("stroke-dasharray"), [])
        # Second line has dots on every "major point"
        self.assertEqual(layer[1].style("stroke-dasharray"), [])
        self.assertEqual(
            layer[1].path,
            inkex.Path(
                "M 0 250 l 8e-05 0 M 16 250 l 8e-05 0 M 24 250 l 8e-05 0 M 48 250 l 8e-05 0"
            ),
        )
        # Third path has dots with a spacing of 5% * distance of P1 and P2 in mm = 200
        self.assertEqual(layer[2].style("stroke-dasharray"), [0, 10])
        # Fourth path has dashes
        self.assertEqual(layer[3].style("stroke-dasharray"), [5, 5])
        # Rectangle is a path with 5mm sized dashes (absolute):
        self.assertEqual(layer[8].style("stroke-dasharray"), [2.5, 2.5])
        # Next rectangle has solid line again
        self.assertEqual(layer[9].style("stroke-dasharray"), [])
        # Next rectangle has dots in the corners
        self.assertEqual(
            layer[10].path,
            inkex.Path(
                "M 48 222 l 8e-05 0 M 64 222 l 8e-05 0 M 64 206 l 8e-05 0 M 48 206 l 8e-05 0 M 48 222 l 8e-05 0"
            ),
        )
        # Next line has a custom line type. Total length must sum up to 5
        self.assertEqual(
            layer[11].style("stroke-dasharray"),
            [0.0, 0.75, 0.0, 0.75, 0.0, 0.75, 2.0, 0.75],
        )
        # Next line: type is reset to the original number 2
        self.assertEqual(layer[12].style("stroke-dasharray"), [5, 5])
        # Next line is again a custom pattern, and afterwards, all line types are reset
        self.assertEqual(layer[14].style("stroke-dasharray"), [5, 5])
