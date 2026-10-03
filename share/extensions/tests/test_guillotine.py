# coding=utf-8
import os
import tarfile
from guillotine import Guillotine
from inkex.tester import ComparisonMixin, TestCase


class GuillotineTester(ComparisonMixin):
    """Test the Guillotine extension"""

    effect_class = Guillotine

    def test_all_comparisons(self):
        """Images are extracted to a file directory"""
        for args in self.comparisons:
            # Create a landing directory for generated images
            outdir = os.path.join(self.tempdir, "img")
            args += ("--directory={}/".format(outdir),)

            # But also set this directory into the compare file
            compare_file = os.path.join(self.tempdir, "compare_file.svg")
            with open(self.data_file(self.compare_file), "rb") as rhl:
                with open(compare_file, "wb") as whl:
                    whl.write(rhl.read().replace(b"{tempdir}", outdir.encode("utf8")))

            self.assertEffect(compare_file, args=args)
            self.assertTrue(os.path.isdir(outdir))

            infile = self.get_compare_cmpfile(args)
            if os.environ.get("EXPORT_COMPARE", False):
                self.export_comparison(outdir, infile)

            with tarfile.open(infile) as tar_handle:
                for item in tar_handle:
                    fileobj = tar_handle.extractfile(item)
                    with open(os.path.join(outdir, item.name), "rb") as fhl:
                        self.assertEqual(
                            fileobj.read(), fhl.read(), "File '{}'".format(item.name)
                        )

    @staticmethod
    def export_comparison(outdir, cmpfile):
        """Export the files as a tar file for manual comparison"""
        tarname = cmpfile + ".export"
        tar = tarfile.open(tarname, "w|")

        # We make a tar archive so we can test it.
        for name in sorted(os.listdir(outdir)):
            with open(os.path.join(outdir, name), "rb") as fhl:
                fhl.seek(0, 2)
                info = tarfile.TarInfo(name)
                info.size = fhl.tell()
                fhl.seek(0)
                tar.addfile(info, fhl)
        tar.close()
        print("Written output: {}.export".format(cmpfile))


class TestGuillotineBasic(GuillotineTester, TestCase):
    stderr_protect = False
    effect_class = Guillotine
    compare_file = "svg/guides.svg"
    comparisons = [
        ("--image=f{}oo",),
        ("--ignore=true",),
    ]


class TestGuillotineMillimeter(GuillotineTester, TestCase):
    stderr_protect = False
    effect_class = Guillotine
    compare_file = "svg/guides_millimeter.svg"
    comparisons = [
        ("--image=output",),
    ]
