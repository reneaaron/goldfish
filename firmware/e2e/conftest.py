import subprocess

import pytest

from device import BINARY, FIRMWARE, Device


@pytest.fixture(scope="session", autouse=True)
def simulator():
    """Build the simulator the tests drive (a no-op when it is up to date)."""
    subprocess.run(["pio", "run", "-e", "sim", "-s"], cwd=FIRMWARE, check=True)
    assert BINARY.exists()


@pytest.fixture
def dev(tmp_path, request):
    """A freshly plugged-in signer: no seed, home screen."""
    d = Device(tmp_path)
    yield d
    try:
        report = getattr(request.node, "rep_call", None)
        if report is not None and report.failed:
            print("\nsigner screen at the failure (%s):\n%s" % (d.screen, d.text()))
            d.screenshot("failure.ppm").save(FIRMWARE / "e2e" / ("failure_%s.png" % request.node.name.replace("/", "_")))
    except RuntimeError as gone:                 # a simulator that died or hung has no screen to show
        print("\nno screen at the failure: %s" % gone)
    finally:
        d.close()


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    setattr(item, "rep_" + outcome.get_result().when, outcome.get_result())
