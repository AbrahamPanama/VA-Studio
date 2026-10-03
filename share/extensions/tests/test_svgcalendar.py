# coding=utf-8
"""
All tests for the svg calendar extension
"""

import calendar
import datetime

from svgcalendar import Calendar
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentStyle, CompareNumericFuzzy
from inkex.tester.mock import MockMixin


class FrozenDateTime(datetime.datetime):
    @classmethod
    def today(cls):
        return cls(2019, 11, 5)


class CalendarArguments(ComparisonMixin, TestCase):
    """Test arguments to calendar extensions"""

    effect_class = Calendar
    compare_filters = [CompareOrderIndependentStyle(), CompareNumericFuzzy()]
    comparisons = [
        (
            "--color-year=#888",
            "--color-month=#666",
            "--color-day-name=#999",
            "--color-day=#000",
            "--color-weekend=#777",
            "--color-nmd=#BBB",
            "--color-weeknr=#808080",
            "--encoding=UTF-8",
        )
    ]
    mocks = [(datetime, "datetime", FrozenDateTime)]

    def test_default_names_list(self):
        """Test default names"""
        effect = self.assertEffect()
        self.assertEqual(effect.options.month_names[0], "January")
        self.assertEqual(effect.options.month_names[11], "December")
        self.assertEqual(effect.options.day_names[0], "Sun")
        self.assertEqual(effect.options.day_names[6], "Sat")
        self.assertEqual(effect.options.year, datetime.datetime.today().year)
        self.assertEqual(calendar.firstweekday(), 6)

    def test_modifyed_names_list(self):
        """Test modified names list"""
        effect = self.assertEffect(
            args=[
                "--month-names=JAN FEV MAR ABR MAI JUN JUL AGO SET OUT NOV DEZ",
                "--day-names=DOM SEG TER QUA QUI SEX SAB",
            ]
        )
        self.assertEqual(effect.options.month_names[0], "JAN")
        self.assertEqual(effect.options.month_names[11], "DEZ")
        self.assertEqual(effect.options.day_names[0], "DOM")
        self.assertEqual(effect.options.day_names[6], "SAB")

    def test_starting_names_list(self):
        """Starting or ending spaces must not affect names"""
        effect = self.assertEffect(
            args=[
                "--month-names= JAN FEV MAR ABR MAI JUN JUL AGO SET OUT NOV DEZ ",
                "--day-names=    DOM SEG TER QUA QUI SEX SAB    ",
            ]
        )
        self.assertEqual(effect.options.month_names[0], "JAN")
        self.assertEqual(effect.options.month_names[11], "DEZ")
        self.assertEqual(effect.options.day_names[0], "DOM")
        self.assertEqual(effect.options.day_names[6], "SAB")

    def test_inner_extra_spaces(self):
        """Extra spaces must not affect names"""
        effect = self.assertEffect(
            args=[
                "--month-names=JAN FEV        MAR ABR MAI JUN JUL AGO SET OUT NOV DEZ",
                "--day-names=DOM SEG        TER QUA QUI SEX SAB",
            ]
        )
        self.assertEqual(effect.options.month_names[0], "JAN")
        self.assertEqual(effect.options.month_names[2], "MAR")
        self.assertEqual(effect.options.month_names[11], "DEZ")
        self.assertEqual(effect.options.day_names[0], "DOM")
        self.assertEqual(effect.options.day_names[2], "TER")
        self.assertEqual(effect.options.day_names[6], "SAB")

    def test_converted_year_zero(self):
        """Year equal to 0 is converted to correct year"""
        effect = self.assertEffect(args=["--year=0"])
        self.assertEqual(effect.options.year, datetime.datetime.today().year)

    def test_converted_year_thousand(self):
        """Year equal to 2000 configuration"""
        effect = self.assertEffect(args=["--year=2000"])
        self.assertEqual(effect.options.year, 2000)

    def test_configuring_week_start_sun(self):
        """Week start is set to Sunday"""
        self.assertEffect(args=["--start-day=sun"])
        self.assertEqual(calendar.firstweekday(), 6)

    def test_configuring_week_start_mon(self):
        """Week start is set to Monday"""
        self.assertEffect(args=["--start-day=mon"])
        self.assertEqual(calendar.firstweekday(), 0)

    def test_recognize_a_weekend(self):
        """Recognise a weekend"""
        effect = self.assertEffect(
            args=[
                "--start-day=sun",
                "--weekend=sat+sun",
            ]
        )
        self.assertTrue(effect.is_weekend(0), "Sunday is weekend in this configuration")
        self.assertTrue(
            effect.is_weekend(6), "Saturday is weekend in this configuration"
        )
        self.assertFalse(effect.is_weekend(1), "Monday is NOT weekend")
