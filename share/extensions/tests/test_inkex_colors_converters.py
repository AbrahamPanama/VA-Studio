# coding=utf-8

from collections import defaultdict

from inkex.colors.color import Color
from inkex.colors.converters import Converters
from inkex.tester import TestCase


class Blue(Color):
    name = "blue"
    scales = ((0, 255), (0, 255), (0, 255), (0.0, 1.0))
    channels = ("red", "green", "blue")


class Red(Blue):
    name = "red"
    scales = ((0, 100), (0, 100), (0, 100), (0.0, 1.0))

    @staticmethod
    def convert_to_blue(red, green, blue, *alpha):
        return [0, green, red] + list(alpha)

    @staticmethod
    def convert_from_blue(red, green, blue, *alpha):
        return [blue, green, 0] + list(alpha)


class Green(Blue):
    name = "green"
    scales = ((100, 200), (100, 200), (100, 200), (0.0, 1.0))

    @staticmethod
    def convert_to_red(red, green, blue, *alpha):
        return [green, 0, blue] + list(alpha)

    @staticmethod
    def convert_from_red(red, green, blue, *alpha):
        return [0, red, blue] + list(alpha)


class Purple(Blue):
    name = "purple"
    scales = ((0, 30), (0, 150), (0, 1.0), (0.0, 1.0))

    @staticmethod
    def convert_to_blue(red, green, blue, *alpha):
        return [0, green, red + blue] + list(alpha)

    @staticmethod
    def convert_from_blue(red, green, blue, *alpha):
        return [blue / 2, 0, blue / 2] + list(alpha)

    @staticmethod
    def convert_from_red(red, green, blue, *alpha):
        return [red / 2, 0, red / 2] + list(alpha)

    def convert_nothing_to_do_with_convert(self):
        pass


class Magenta(Blue):
    name = "magenta"


class MyConverters(Converters):
    links = defaultdict(dict)
    chains = {}


class ColorConverters(TestCase):
    """Test builfing process"""

    def setUp(self):
        MyConverters.links.clear()
        MyConverters.chains.clear()
        MyConverters.add_space(Red)
        MyConverters.add_space(Blue)
        MyConverters.add_space(Green)
        MyConverters.add_space(Purple)

    def test_01_adding(self):
        self.assertEqual(len(MyConverters.links), 4)
        self.assertEqual(sum([len(v) for v in MyConverters.links.values()]), 7)

    def test_02_add_bad(self):
        class Bad:
            name = "bad"

            def convert_to_red(values):
                pass

        self.assertRaises(TypeError, MyConverters.add_space, Bad)

        func = MyConverters.find_converter(Magenta, Blue)
        self.assertRaises(NotImplementedError, func, None)

    def test_04_build_chain(self):
        """Basic conversions"""
        self.assertEqual(MyConverters.get_chain("apple", "blue"), None)
        self.assertEqual(MyConverters.get_chain("red", "orange"), None)
        self.assertEqual(MyConverters.get_chain("red", "red"), ["red"])
        self.assertEqual(MyConverters.get_chain("red", "blue"), ["red", "blue"])
        self.assertEqual(MyConverters.get_chain("blue", "red"), ["blue", "red"])
        self.assertEqual(MyConverters.get_chain("red", "green"), ["red", "green"])

    def test_05_build_multiple(self):
        """Multiple hops between conversions"""
        self.assertEqual(
            MyConverters.get_chain("blue", "green"), ["blue", "red", "green"]
        )
        self.assertEqual(
            MyConverters.get_chain("green", "blue"), ["green", "red", "blue"]
        )

    def test_06_build_shortcut(self):
        """Can convert down and up a stack"""
        self.assertEqual(
            MyConverters.get_chain("green", "purple"), ["green", "red", "purple"]
        )

    def test_07_forward(self):
        func = MyConverters.find_converter(Red, Blue)
        self.assertEqual(func([100, 0, 0]), [0, 0, 255])
        func = MyConverters.find_converter(Red, Green)
        self.assertEqual(func([100, 0, 0]), [100, 200, 100])

    def test_08_backward(self):
        func = MyConverters.find_converter(Blue, Red)
        self.assertEqual(func([0, 0, 128]), [50, 0, 0])
        func = MyConverters.find_converter(Green, Red)
        color = func([0, 150, 0, 0.5])
        self.assertTrue(isinstance(color, Red))
        self.assertEqual(color, [50, 0, 0])
        self.assertEqual(color.alpha, 0.5)

    def test_09_multiple(self):
        func = MyConverters.find_converter(Blue, Green)
        self.assertEqual(func([0, 0, 64]), [100, 125, 100])
        func = MyConverters.find_converter(Green, Blue)
        color = func([150, 150, 150, 0.5])
        self.assertTrue(isinstance(color, Blue))
        self.assertEqual(func([150, 150, 150, 0.5]), [0, 0, 128])
        self.assertEqual(color.alpha, 0.5)

    def test_10_constrain(self):
        func = MyConverters.find_converter(Red, Blue)
        self.assertEqual(func([-50, 150, 0]), [0, 255, 0])
