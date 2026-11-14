"""Surface-local occlusion representations; no camera input or runtime traversal.

The angular models below are offline fitted prototypes, not neural decoders.
They define a common query/selection contract for future learned estimators.
"""
from dataclasses import dataclass
from enum import IntEnum
import numpy as np


class Estimator(IntEnum):
    CURRENT_ROUTING = 0
    EXISTING_RAY_NEURAL = 1
    DIRECTIONAL_FIELD_32 = 2
    ANGULAR_OCCLUDERS_4 = 3
    ANGULAR_OCCLUDERS_8 = 4
    DETAILED_REFERENCE = 5  # Inspection/teacher only, never a runtime fallback.


REACH_CAPS = (.025, .1, .3, 0.0)  # Last is relief-top exit, NOT zero distance.


def normalize(values):
    values = np.asarray(values, np.float64)
    length = np.linalg.norm(values, axis=-1, keepdims=True)
    if not np.isfinite(values).all() or np.any(length < 1e-12):
        raise ValueError("Zero-length direction")
    return values / length


def cosine_directions(count):
    i = np.arange(count)
    angle = i * np.pi * (3 - np.sqrt(5))
    r = np.sqrt((i + .5) / count)
    return np.column_stack((r * np.cos(angle), r * np.sin(angle), np.sqrt(1 - r * r)))


def query_directions(count, seed):
    rng = np.random.default_rng(seed)
    # Include broad angles and shallow lights independently of ambient weight.
    z = rng.uniform(.025, 1, count)
    z[:count // 3] = rng.uniform(.015, .20, count // 3)
    angle = rng.uniform(0, 2 * np.pi, count)
    r = np.sqrt(1 - z * z)
    return np.column_stack((r * np.cos(angle), r * np.sin(angle), z))


def directional_field(values, directions):
    """Nearest-direction field query; exposes its angular discretization."""
    canonical = cosine_directions(values.shape[-1])
    indices = np.argmax(normalize(directions) @ canonical.T, axis=-1)
    return np.take(values, indices, axis=-1)


def pack_blocked(visibility):
    """Packing only: integer values are never regression targets."""
    visibility = np.asarray(visibility)
    if visibility.ndim != 1 or len(visibility) > 32 or not np.isin(visibility, [0, 1]).all():
        raise ValueError("A binary field of at most 32 directions is required")
    return sum((1 << i) for i, value in enumerate(visibility) if value == 0)


@dataclass
class AngularOcclusion:
    axes: np.ndarray             # K normalized surface-local directions
    cosine: np.ndarray           # K x reach bands; nonincreasing with reach
    edge_beta: float = 0.0       # 0 = binary point union; >0 = fitted footprint coverage

    def __post_init__(self):
        self.axes = normalize(self.axes)
        self.cosine = np.asarray(self.cosine, np.float64)
        if self.cosine.ndim != 2 or self.cosine.shape[0] != len(self.axes):
            raise ValueError("One radius per occluder and reach band is required")
        if not np.isfinite(self.cosine).all() or not np.isfinite(self.edge_beta) or self.edge_beta < 0:
            raise ValueError("Nonfinite or negative model parameters")
        if np.any((self.cosine < 0) | (self.cosine > 1)) or np.any(np.diff(self.cosine, axis=1) > 1e-10):
            raise ValueError("Reach bands must have nested angular coverage")

    def visibility(self, directions):
        directions = normalize(directions)
        dots = directions @ self.axes.T
        delta = dots[None, :, :] - self.cosine.T[:, None, :]
        if self.edge_beta == 0:
            return (~np.any(delta > 0, axis=-1)).astype(np.float64)
        # The fitted target is an explicit receiver-footprint visibility fraction,
        # not confidence in a binary prediction.
        # Filter the union boundary, not each primitive independently. Taking
        # the maximum margin is idempotent: overlapping/duplicate blockers do
        # not darken the same direction twice.
        maximum = self.edge_beta * np.max(delta, axis=-1)
        return np.exp(-np.logaddexp(0, maximum))

    def to_dict(self):
        return {"axes": self.axes.tolist(), "cosine": self.cosine.tolist(), "edge_beta": self.edge_beta}


@dataclass
class EstimatorSet:
    """Explicit selector contract; unavailable choices never silently fall back.

    Callbacks are surface-local and return reach-bands x directions. A future
    neural decoder supplies these same field/occluder representations once per
    receiver; individual lights query them without a second inference.
    """
    reference: object = None
    current_routing: object = None
    ray_neural: object = None
    field32: object = None
    angular4: object = None
    angular8: object = None

    def evaluate(self, estimator, directions, reach_band=None):
        estimator = Estimator(estimator)
        directions = normalize(directions)
        if estimator == Estimator.CURRENT_ROUTING:
            result = None if self.current_routing is None else self.current_routing(directions)
        elif estimator == Estimator.DETAILED_REFERENCE:
            result = None if self.reference is None else self.reference(directions)
        elif estimator == Estimator.EXISTING_RAY_NEURAL:
            result = None if self.ray_neural is None else self.ray_neural(directions)
        elif estimator == Estimator.DIRECTIONAL_FIELD_32:
            if self.field32 is not None and np.asarray(self.field32).shape[-1] != 32:
                raise ValueError("The selected directional field requires 32 components")
            result = None if self.field32 is None else directional_field(np.asarray(self.field32), directions)
        else:
            model = self.angular4 if estimator == Estimator.ANGULAR_OCCLUDERS_4 else self.angular8
            expected = 4 if estimator == Estimator.ANGULAR_OCCLUDERS_4 else 8
            if model is not None and len(model.axes) != expected:
                raise ValueError("Occluder capacity does not match the selected estimator")
            result = None if model is None else model.visibility(directions)
        if result is None:
            raise ValueError("Selected estimator is unavailable: " + estimator.name)
        if reach_band is not None:
            if reach_band not in range(len(REACH_CAPS)):
                raise ValueError("The prototype requires an explicit supported reach band")
            return result[reach_band]
        return result


def _initial(labels, directions, count, seed):
    rng = np.random.default_rng(seed)
    blocked = directions[labels[-1] < .5]
    if not len(blocked):
        blocked = directions
    chosen = [int(rng.integers(len(blocked)))]
    for _ in range(1, count):
        distance = np.maximum(0, 1 - np.max(blocked @ blocked[chosen].T, axis=1))
        chosen.append(int(rng.choice(len(blocked), p=(distance + 1e-8) / (distance + 1e-8).sum())))
    axes = blocked[chosen].copy()
    for _ in range(10):
        group = np.argmax(blocked @ axes.T, axis=1)
        for j in range(count):
            if np.any(group == j):
                axes[j] = normalize(blocked[group == j].mean(axis=0))
    # Below-horizon centers can represent long, low angular blockers.
    axes[:, 2] -= .1 * (seed % 3)
    axes = normalize(axes)
    dots = directions @ axes.T
    assigned = np.argmax(dots, axis=1)
    cosine = np.ones((count, len(labels))) * .99999
    for j in range(count):
        for band in range(len(labels)):
            keep = (assigned == j) & (labels[band] < .5)
            if keep.any():
                cosine[j, band] = np.clip(np.quantile(dots[keep, j], .10), 0, .99999)
    return axes, np.sort(cosine, axis=1)[:, ::-1].copy()


def fit_angular(directions, labels, calibration_directions, calibration_labels,
                count, filtered=False, steps=600, restarts=3, seed=9182):
    """Fit the COMBINED directional coverage, never primitive identities or AO alone.

    Only calibration directions select a restart. Held-out directions are never
    used for fitting or selection. Shared axes and ordered radii enforce reach
    monotonicity without adding occluders at longer reaches.
    """
    directions = normalize(directions)
    labels = np.asarray(labels, np.float64)
    known = labels >= 0
    weight = known / max(1, known.sum())
    target = np.maximum(labels, 0)
    best, best_score = None, np.inf
    for restart in range(restarts):
        axes, cosine = _initial(np.where(known, labels, 1), directions, count, seed + restart)
        moment = [np.zeros_like(axes), np.zeros_like(cosine)]
        variance = [np.zeros_like(axes), np.zeros_like(cosine)]
        for step in range(1, steps + 1):
            beta = 32.0 if filtered else 12 + 84 * step / steps
            delta = directions @ axes.T
            z = beta * (delta[None, :, :] - cosine.T[:, None, :])
            if filtered:
                winner = np.argmax(z, axis=-1)
                maximum = np.max(z, axis=-1)
                visible = np.exp(-np.logaddexp(0, maximum))
                active = np.arange(count)[None, None, :] == winner[:, :, None]
                gradient = weight[:, :, None] * (target - visible)[:, :, None] * active
            else:
                blocked = 1 / (1 + np.exp(-np.clip(z, -40, 40)))
                visible = np.exp(-np.logaddexp(0, z).sum(axis=-1))
                gradient = weight[:, :, None] * ((target - visible) / np.maximum(1 - visible, 1e-8))[:, :, None] * blocked
            axis_gradient = beta * np.einsum("bnk,nd->kd", gradient, directions)
            axis_gradient -= axes * np.sum(axis_gradient * axes, axis=1, keepdims=True)
            cosine_gradient = -beta * gradient.sum(axis=1).T
            rate = .012 if step < steps * .7 else .004
            for i, (parameter, g) in enumerate(((axes, axis_gradient), (cosine, cosine_gradient))):
                moment[i] = .9 * moment[i] + .1 * g
                variance[i] = .999 * variance[i] + .001 * g * g
                parameter -= rate * moment[i] / (1 - .9 ** step) / (np.sqrt(variance[i] / (1 - .999 ** step)) + 1e-8)
            axes = normalize(axes)
            cosine = np.sort(np.clip(cosine, 0, .99999), axis=1)[:, ::-1].copy()
        model = AngularOcclusion(axes, cosine, 32.0 if filtered else 0.0)
        predicted = model.visibility(calibration_directions)
        valid = calibration_labels >= 0
        score = np.abs(predicted - calibration_labels)[valid].mean()
        if score < best_score:
            best, best_score = model, float(score)
    return best, {"calibration_mae": best_score, "steps": steps, "restarts": restarts}


def metrics(predicted, reference, directions):
    valid = reference >= 0
    error = np.abs(predicted - reference)
    shallow = np.broadcast_to(np.asarray(directions)[:, 2] < .2, valid.shape) & valid
    return {"queries": int(valid.sum()), "unknown": int((~valid).sum()),
            "mae": float(error[valid].mean()),
            "shallow_mae": float(error[shallow].mean()) if shallow.any() else None,
            "false_lit": int(((predicted >= .5) & (reference < .5) & valid).sum()),
            "false_shadow": int(((predicted < .5) & (reference >= .5) & valid).sum())}
