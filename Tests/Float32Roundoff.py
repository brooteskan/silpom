"""Explicit bounded-FP32 comparison; ownership, metadata and clear values stay exact."""
import numpy as np


def compare_components(before, after, owned, float_columns, max_ulps):
    assert before.dtype == after.dtype == np.dtype('<u4') and before.shape == after.shape
    assert 0 <= max_ulps <= 2 and 0 < float_columns <= before.shape[-1]
    assert np.array_equal(before[..., float_columns:], after[..., float_columns:]), 'Non-float fields changed'
    assert np.array_equal(before[~owned], after[~owned]), 'Unowned/clear values changed'
    if max_ulps == 0:
        assert np.array_equal(before, after), 'Bitwise comparison failed'
    a, b = before[..., :float_columns][owned], after[..., :float_columns][owned]
    af, bf = a.view('<f4'), b.view('<f4')
    assert np.isfinite(af).all() and np.isfinite(bf).all(), 'Nonfinite float value'
    def ordered(bits):
        magnitude = (bits & np.uint32(0x7fffffff)).astype(np.int64)
        return np.where((bits >> np.uint32(31)) != 0, 0x80000000 - magnitude, 0x80000000 + magnitude)
    distance = np.abs(ordered(a) - ordered(b))
    maximum = int(distance.max(initial=0))
    assert maximum <= max_ulps, ('Float roundoff exceeds explicit limit', maximum, max_ulps)
    return dict(different_owned_samples=int(np.count_nonzero(np.any(a != b, axis=-1))),
                max_component_ulps=maximum,
                max_abs=float(np.abs(af.astype(np.float64) - bf.astype(np.float64)).max(initial=0)))
