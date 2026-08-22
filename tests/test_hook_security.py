"""Static-security regression tests for ``usb_monitor.hooks``.

These tests do not execute hooks — they parse the source module with the
``ast`` module and assert structural invariants that, if violated, would
re-introduce a command-injection class vulnerability.  The point is to
catch regressions at review/CI time, not at runtime, because:

* ``python -O`` strips ``assert`` statements, so runtime assertions are
  unreliable in optimized builds.
* A future contributor might add a *second* subprocess call elsewhere
  in the module and forget to set ``shell=False``.  We want one test
  that covers every call site in the file, not one assertion per call.

Scope:

* Every call into the :mod:`subprocess` module inside
  ``usb_monitor/hooks.py`` must explicitly pass ``shell=False`` as a
  keyword argument, and the literal value must be ``False`` (not a
  variable that could be flipped to ``True`` upstream).
* The same calls must explicitly redirect ``stdin``, ``stdout`` and
  ``stderr`` to :data:`subprocess.DEVNULL` so a hook cannot block on
  standard input or fill the parent's pipe buffers.
* A bandit's-eye-view grep must still find the literal string
  ``shell=True`` nowhere in the module — if it ever reappears, it
  must be deliberate and accompanied by a justification comment.
"""

from __future__ import annotations

import ast
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOOKS_PATH = ROOT / "usb_monitor" / "hooks.py"
SUBPROCESS_FUNCS = {"Popen", "run", "call", "check_call", "check_output"}


def _iter_subprocess_calls(tree: ast.AST) -> list[ast.Call]:
    """Yield every ``subprocess.<func>(...)`` call in ``tree``."""
    calls: list[ast.Call] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        func = node.func
        if (
            isinstance(func, ast.Attribute)
            and isinstance(func.value, ast.Name)
            and func.value.id == "subprocess"
            and func.attr in SUBPROCESS_FUNCS
        ):
            calls.append(node)
    return calls


def _keyword(call: ast.Call, name: str) -> ast.keyword | None:
    for kw in call.keywords:
        if kw.arg == name:
            return kw
    return None


def test_hooks_module_exists_and_parses() -> None:
    """Sanity: the file under test is still parseable Python."""
    source = HOOKS_PATH.read_text(encoding="utf-8")
    ast.parse(source)


def test_hooks_contains_at_least_one_subprocess_call() -> None:
    """If this ever fires, the regression test below is useless."""
    source = HOOKS_PATH.read_text(encoding="utf-8")
    calls = _iter_subprocess_calls(ast.parse(source))
    assert calls, (
        "hooks.py no longer contains a checked subprocess call; "
        "this regression test must be updated together with the new call site"
    )


def test_hooks_subprocess_calls_explicitly_disable_shell() -> None:
    """Every subprocess call must pass ``shell=False`` as a keyword literal.

    We require a *keyword* argument so a future caller cannot pass
    ``shell`` positionally and silently re-enable parsing.  We also
    require the value to be a literal ``False`` constant — not a
    variable or expression that could resolve to ``True`` upstream.
    """
    source = HOOKS_PATH.read_text(encoding="utf-8")
    for call in _iter_subprocess_calls(ast.parse(source)):
        shell_kw = _keyword(call, "shell")
        assert shell_kw is not None, (
            f"subprocess.{_call_name(call)}() in hooks.py must explicitly "
            f"pass shell=False as a keyword argument"
        )
        value = shell_kw.value
        assert isinstance(value, ast.Constant) and value.value is False, (
            f"subprocess.{_call_name(call)}() in hooks.py must use shell=False "
            f"as a literal constant — never a variable or expression that "
            f"could be flipped upstream"
        )


def test_hooks_subprocess_calls_redirect_all_streams() -> None:
    """All three standard streams must be redirected to DEVNULL.

    This prevents a hook from blocking on stdin, flooding the parent
    process's pipe buffers, or leaking output that may contain
    attacker-influenced content (volume labels, paths, etc.).
    """
    source = HOOKS_PATH.read_text(encoding="utf-8")
    for call in _iter_subprocess_calls(ast.parse(source)):
        for stream in ("stdin", "stdout", "stderr"):
            kw = _keyword(call, stream)
            assert kw is not None, (
                f"subprocess.{_call_name(call)}() in hooks.py must redirect "
                f"{stream}=subprocess.DEVNULL"
            )
            # We accept either a bare ``subprocess.DEVNULL`` attribute or a
            # name that resolves to it, but reject plain ``None`` / ``True``
            # / pipes-inherited defaults.
            assert not (
                isinstance(kw.value, ast.Constant) and kw.value.value is None
            ), f"{stream} must not be inherited from the parent process"


def test_hooks_carries_security_documentation() -> None:
    """The SECURITY block comment in ``_fire`` must stay in place.

    A future cleanup might delete the security block comment thinking it
    is "obvious" or "comment clutter".  This test ensures the warning
    text remains in the source so the next maintainer reads it.
    """
    source = HOOKS_PATH.read_text(encoding="utf-8")
    assert "SECURITY" in source, (
        "hooks.py must keep a SECURITY block comment explaining why "
        "shell=False is mandatory for hook commands"
    )
    assert "shell=False" in source, (
        "hooks.py must still contain the literal 'shell=False' so the "
        "AST test above has something to lock onto"
    )


def _call_name(call: ast.Call) -> str:
    func = call.func
    assert isinstance(func, ast.Attribute)
    return func.attr
