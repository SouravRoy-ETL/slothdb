"""
Setup shim that forces a platform-tagged wheel.

The ctypes wrapper is pure Python but loads slothdb.dll/.so/.dylib at
runtime, so the wheel must be tagged for the platform that produced the
binary (e.g. py3-none-win_amd64) — not py3-none-any. Without this shim
setuptools tags the wheel as pure-Python and pip will happily install
the Windows DLL on a Linux user, which is what shipped in 0.1.7.
"""
from setuptools import setup

try:
    from setuptools.command.bdist_wheel import bdist_wheel
except ImportError:
    from wheel.bdist_wheel import bdist_wheel


class PlatformWheel(bdist_wheel):
    def finalize_options(self):
        super().finalize_options()
        self.root_is_pure = False

    def get_tag(self):
        _python, _abi, plat = super().get_tag()
        return "py3", "none", plat


setup(cmdclass={"bdist_wheel": PlatformWheel})
