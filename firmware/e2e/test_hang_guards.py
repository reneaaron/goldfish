"""A simulator that stops answering: the test fails within seconds, says on
which command, and no simulator is left running. (These guard the tests; they
don't keep the firmware from getting stuck.)"""
import pytest

from device import Device


def test_stuck_simulator_gives_up_on_itself(tmp_path):
    """Its own watchdog: it exits, and the command reports it gone."""
    d = Device(tmp_path, env={"SIM_WATCHDOG": "1"})
    with pytest.raises(RuntimeError, match="died on: hang"):
        d.cmd("hang")
    assert d.proc.wait(timeout=5) == 70
    d.close()


def test_command_without_an_answer_fails_and_kills_it(tmp_path):
    """Without the watchdog: the command gives up after its time and kills it."""
    d = Device(tmp_path, reply_s=1, env={"SIM_WATCHDOG": "0"})
    with pytest.raises(RuntimeError, match="hung on: hang"):
        d.cmd("hang")
    assert d.proc.wait(timeout=5) != 0
    d.close()


def test_close_kills_a_stuck_simulator(tmp_path):
    d = Device(tmp_path, env={"SIM_WATCHDOG": "0"})
    d.proc.stdin.write("hang\n")
    d.proc.stdin.flush()
    d.close(grace_s=1)
    assert d.proc.poll() is not None


def test_healthy_simulator_is_unaffected(tmp_path):
    d = Device(tmp_path, env={"SIM_WATCHDOG": "1"})
    d.wait(5000)                                 # the fake clock: no real seconds pass
    assert d.screen == "HOME"
    d.close()
    assert d.proc.returncode == 0
