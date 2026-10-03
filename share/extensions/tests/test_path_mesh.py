# coding=utf-8

from path_mesh_m2p import MeshToPath
from path_mesh_p2m import PathToMesh

from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy


class PathToMeshTest(ComparisonMixin, TestCase):
    """Test path to mesh with comparisons"""

    effect_class = PathToMesh
    comparisons = [
        ("--id=path1", "--id=path9"),
    ]
    compare_file = "svg/mesh.svg"


class MeshToPathTest(ComparisonMixin, TestCase):
    """Test mesh to path with comparisons"""

    compare_filters = [CompareNumericFuzzy()]
    effect_class = MeshToPath
    comparisons = [
        ("--id=mesh1", "--mode=outline"),
        ("--id=mesh1", "--mode=gridlines"),
        ("--id=mesh1", "--mode=meshpatches"),
        ("--id=mesh1", "--mode=faces"),
    ]
    compare_file = "svg/mesh.svg"
