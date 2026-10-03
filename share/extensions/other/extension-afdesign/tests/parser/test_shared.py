# SPDX-FileCopyrightText: 2024 Manpreet Singh <manpreet.singh.dev@proton.me>
#
# SPDX-License-Identifier: GPL-2.0-or-later

from typing import Optional

import pytest
import gc
from inkaf.parser.sharedaf import (
    DocIdInvalidError,
    LinkedObjectNotFound,
    SharedAFDictObject,
)
from inkaf.parser.types import Field, ObjectStatus


def af(id: int, doc_id: Optional[int] = None) -> SharedAFDictObject:
    return SharedAFDictObject(
        id=id, types=[], status=ObjectStatus.NULL, fields={}, docid=doc_id
    )


def test_linking():
    """
    Verify that shared fields are accessible from link objects.
    """
    af1 = af(0)
    af2 = af(0, doc_id=af1.doc_id)
    af1.fields["f0"] = Field(0, "test")

    af1.set_status(ObjectStatus.SHARED)
    assert af1["f0"] == "test"
    with pytest.raises(KeyError):
        _ = af2["f0"]

    af2.set_status(ObjectStatus.LINK)
    assert af2["f0"] == af1["f0"] and af1["f0"] == "test"


def test_unlinking():
    """
    Verify that after unsharing, shared fields are no longer accessible from linked objects.
    """
    af1 = af(0)
    af2 = af(0, doc_id=af1.doc_id)
    af1.fields["f0"] = Field(0, "test")

    af2.set_status(ObjectStatus.LINK)
    assert af1["f0"] == "test"
    with pytest.raises(LinkedObjectNotFound):
        _ = af2["f0"]

    af1.set_status(ObjectStatus.SHARED)
    assert af2["f0"] == af1["f0"] and af1["f0"] == "test"

    af1.set_status(ObjectStatus.NULL)
    assert af1["f0"] == "test"
    with pytest.raises(LinkedObjectNotFound):
        _ = af2["f0"]


def test_no_inter_doc_link():
    """
    Verify that shared fields are not accessible from different documents.
    """
    af1 = af(0, doc_id=None)
    af2 = af(0, doc_id=None)
    af1.fields["f0"] = Field(0, "test")

    af1.set_status(ObjectStatus.SHARED)
    af2.set_status(ObjectStatus.LINK)
    assert af1.doc_id != af2.doc_id
    assert af1["f0"] == "test"
    with pytest.raises(LinkedObjectNotFound):
        _ = af2["f0"]


def test_contains():
    """
    Test the behavior of __contains__.
    """
    af1 = af(0)
    af2 = af(0, doc_id=af1.doc_id)

    assert "f0" not in af1
    assert "f0" not in af2

    af1.fields["f0"] = Field(0, "test")
    assert "f0" in af1
    assert "f0" not in af2

    af1.set_status(ObjectStatus.SHARED)
    af2.set_status(ObjectStatus.LINK)
    assert "f0" in af1
    assert "f0" in af2


def test_invalid_doc():
    """
    Ensure that attempting to create an object with an invalid doc_id
    raises `DocIdInvalidError`.
    """
    with pytest.raises(DocIdInvalidError):
        _ = af(0, doc_id=len(SharedAFDictObject._shared_items))

    af1 = af(0)
    with pytest.raises(DocIdInvalidError):
        af1._doc_id = len(SharedAFDictObject._shared_items)
        af1.set_status(ObjectStatus.SHARED)


def test_mem_leak():
    """
    Ensure that a shared object with no references is garbage collected.
    """
    af1 = af(0)
    af2 = af(0, doc_id=af1.doc_id)

    af1.set_status(ObjectStatus.SHARED)
    af2.set_status(ObjectStatus.LINK)

    del af1
    gc.collect()

    with pytest.raises(LinkedObjectNotFound):
        _ = af2["f0"]
    assert af2.id not in af2.shared_items
