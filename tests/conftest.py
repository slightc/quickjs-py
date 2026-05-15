import pytest

import quickjs


@pytest.fixture
def ctx():
    """A fresh QuickJS context with its own private runtime."""
    return quickjs.Context()


@pytest.fixture
def rt():
    """A fresh QuickJS runtime."""
    return quickjs.Runtime()
