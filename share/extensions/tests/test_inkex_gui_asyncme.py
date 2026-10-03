#
# Copyright 2015 Ian Denhardt <ian@zenhack.net>
#           2022 Martin Owens <doctormo@geek-2.com>
#
# This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>
#
"""
Test async threading code
"""

import sys
import time
import pytest
import threading

from inkex.tester import TestCase
from inkex.utils import DependencyError

try:
    from inkex.gui import asyncme
except DependencyError:
    asyncme = None


@pytest.mark.skipif(asyncme is None, reason="PyGObject is required")
class AsyncTest(TestCase):
    """Test the gui async code"""

    def test_basic(self):
        """Basic waiting"""
        future = asyncme.Future()
        self.assertFalse(future.is_ready())

        def do_result():
            future.result("ok")

        thread = asyncme.spawn_thread(do_result)
        self.assertEqual(future.wait(), "ok")
        self.assertTrue(future.is_ready())
        thread.join()

    def test_exception(self):
        """Exception handling"""

        def do_exception():
            raise IOError("It broke!")

        future = asyncme.Future()
        future.run(do_exception)
        self.assertRaises(IOError, future.wait)

    def test_holding(self):
        self._test_holding(True, 0.5, 4)
        self._test_holding(False, 2, 1000)

    def _test_holding(self, blocking, delay, count):
        """Test holding for a delay"""
        lock = threading.Lock()
        shared_var = [0]

        def do_thread():
            self.assertEqual(shared_var[0], 0)
            shared_var[0] += 1
            time.sleep(delay)
            self.assertEqual(shared_var[0], 1)
            shared_var[0] -= 1

        results = []
        for i in range(count):
            results.append(asyncme.holding(lock, do_thread, blocking))

        for r in results:
            if r is not None:
                r.wait()

    def test_debounce(self):
        """Put DebounceSyncVar through its paces

        We create a dsv and with a delay of 1 second and launch two threads
        in parallel. One thread collects values from the dsv repeatedly.
        The other submits ten values, the first five with replace(), the rest
        with put().

        Unless the machine this is running on is very slow, the final result
        should be the last value that was inserted via replace(), followed by
        all of the values inserted with put(); The one-second delay guarantees
        that the first four values will be overwritten.

        This should take about 6 seconds to run.
        """
        dsv = asyncme.DebouncedSyncVar(2)
        dsv.set_delay(1)

        def do_replace_put():
            for i in range(0, 5):
                dsv.replace(i)
            for i in range(5, 10):
                dsv.put(i)

        future = asyncme.Future()

        def do_get():
            result = []
            i = 0
            while i < 9:
                i, _ok = dsv.get()
                result.append(i)
            future.result(result)

        asyncme.spawn_thread(do_replace_put)
        asyncme.spawn_thread(do_get)
        result = future.wait()
        self.assertEqual(result, list(range(4, 10)))
