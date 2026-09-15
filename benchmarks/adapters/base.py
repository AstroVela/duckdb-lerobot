"""Adapter outputs carry actual frame keys and an explicit image layout."""

from dataclasses import dataclass
from typing import Any


@dataclass
class Frame:
    key: list
    image: Any
    layout: str


def chunks(values, size):
    for offset in range(0, len(values), size):
        yield values[offset : offset + size]
