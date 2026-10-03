# coding=utf-8
from synfig_fileformat import defaultLayerVersion


class TestSynFigDefaultLayerVersion(object):
    def test_layer_version(self):
        assert defaultLayerVersion("outline") == "0.2"
