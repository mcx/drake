from pydrake.common.containers import EqualToDict, namedview, NamedViewBase

import unittest

import numpy as np


class Comparison:
    def __init__(self, lhs, rhs):
        self.lhs = lhs
        self.rhs = rhs

    def __bool__(self):
        raise ValueError("Should not be called")

    __nonzero__ = __bool__


class Item:
    equal_to_called = False

    def __init__(self, value):
        self.value = value

    def __hash__(self):
        return hash(self.value)

    def __eq__(self, other):
        # Non-boolean return value.
        return Comparison(self.value, other.value)

    def EqualTo(self, other):
        Item.equal_to_called = True
        return hash(self) == hash(other)


# Globals for testing.
a = Item(1)
b = Item(2)


class TestEqualToDict(unittest.TestCase):
    def test_normal_dict(self):
        d = {a: "a", b: "b"}
        # TODO(eric.cousineau): Figure out how to reproduce failure when `dict`
        # attempts to use `__eq__`, similar to what happens when using
        # `Polynomial` as a key in a dictionary.
        self.assertEqual(d[a], "a")
        with self.assertRaises(ValueError):
            value = bool(a == b)

    def test_equal_to_dict(self):
        d = EqualToDict({a: "a", b: "b"})
        # Ensure that we call `EqualTo`.
        self.assertFalse(Item.equal_to_called)
        self.assertEqual(d[a], "a")
        self.assertTrue(Item.equal_to_called)

        self.assertEqual(d[b], "b")
        self.assertTrue(a in d)

        # Ensure hash collision does not occur.
        self.assertEqual(hash(a.value), hash(a))
        self.assertFalse(a.value in d)

        # Obtaining the original representation (e.g. for `pybind11`):
        # - Constructing using `dict` will not be what is desired; the keys at
        # present are not directly convertible, thus would create an error.
        # N.B. At present, this behavior may not be overridable via Python, as
        # copying is done via `dict.update`, which has a special case for
        # `dict`-inheriting types which does not have any hooks for key
        # transformations.
        raw_attempt = dict(d)
        keys = list(raw_attempt.keys())
        self.assertFalse(isinstance(keys[0], Item))
        # - Calling `raw()` should provide the desired behavior.
        raw = d.raw()
        keys = list(raw.keys())
        self.assertTrue(isinstance(keys[0], Item))


def is_same_array(a, b):
    # Indicates that two arrays (of the same shape and type) are views into the
    # same memory.
    # See: https://stackoverflow.com/a/55660651/7829525
    return (a.ctypes.data == b.ctypes.data and a.shape == b.shape
            and a.dtype == b.dtype and (a == b).all())


class TestNamedView(unittest.TestCase):
    def test_meta(self):
        a = np.array([1, 2])
        self.assertTrue(is_same_array(a, a))
        self.assertTrue(is_same_array(a, a[:]))
        self.assertTrue(is_same_array(a, np.asarray(a)))
        b = a.copy()
        self.assertFalse(is_same_array(a, b))
        # Show that `np.array()` by default copies the data regardless, and
        # thus is not the same as `np.asarray()`.
        c = np.array(a)
        self.assertFalse(is_same_array(a, c))
        d = np.array(a, copy=False)
        self.assertTrue(is_same_array(a, d))

    def test_array(self):
        MyView = namedview("MyView", ["a", "b"])
        self.assertTrue(issubclass(MyView, NamedViewBase))
        self.assertEqual(MyView.__name__, "MyView")
        self.assertEqual(MyView.get_fields(), ("a", "b"))
        value = np.array([1.0, 2.0])
        view = MyView(value)
        self.assertEqual(view.a, 1.0)
        self.assertEqual(view.b, 2.0)
        self.assertTrue(is_same_array(value, np.asarray(view)))
        self.assertTrue(is_same_array(value, view[:]))
        view.a = 10.0
        self.assertEqual(value[0], 10.0)
        value[1] = 100.0
        self.assertEqual(view.b, 100.0)
        view[:] = 3.0
        np.testing.assert_equal(value, [3.0, 3.0])
        self.assertEqual(str(view), "MyView(a=3.0, b=3.0)")
        self.assertEqual(repr(view),
                         f"<MyView(a={repr(view.a)}, b={repr(view.b)})>")
        with self.assertRaisesRegex(AttributeError, ".*('a', 'b').*"):
            view.c = 42

    def test_Zero(self):
        MyView = namedview("MyView", ["a", "b"])
        view = MyView.Zero()
        self.assertEqual(len(view), 2)
        self.assertEqual(view.a, 0)
        self.assertEqual(view.b, 0)

    def test_name_sanitation(self):
        MyView = namedview("MyView",
                           ["$xyz_base", "iiwa::iiwa", "no spaces", "2vär"])
        self.assertEqual(MyView.get_fields(),
                         ("_xyz_base", "iiwa_iiwa", "no_spaces", "_2vär"))
        view = MyView.Zero()
        view._xyz_base = 3
        view.iiwa_iiwa = 4
        view.no_spaces = 5
        view._2vär = 6
        np.testing.assert_equal(view[:], [3, 4, 5, 6])

        MyView = namedview("MyView", ["$xyz_base", "iiwa::iiwa"],
                           sanitize_field_names=False)
        self.assertEqual(MyView.get_fields(), ("$xyz_base", "iiwa::iiwa"))

    def test_uniqueness(self):
        with self.assertRaisesRegex(AssertionError, ".*must be unique.*"):
            namedview("MyView", ['a', 'a'])
        with self.assertRaisesRegex(AssertionError, ".*must be unique.*"):
            namedview("MyView", ['a_a', 'a__a'])
