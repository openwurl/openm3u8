from os.path import abspath, dirname, exists, join

from setuptools import setup

long_description = None
if exists("README.md"):
    with open("README.md") as file:
        long_description = file.read()

install_reqs = [
    req for req in open(abspath(join(dirname(__file__), "requirements.txt")))
]

# Try to use CFFI for building the C extension
# If CFFI is not available, the package will still work using the pure Python parser
try:
    from m3u8.cparser.build_ffi import ffibuilder
    cffi_modules = ["m3u8/cparser/build_ffi.py:ffibuilder"]
    setup_requires = ["cffi>=1.0.0"]
except ImportError:
    cffi_modules = []
    setup_requires = []

setup(
    name="m3u8",
    author="Globo.com",
    version="6.3.0",
    license="MIT",
    zip_safe=False,
    include_package_data=True,
    install_requires=install_reqs,
    setup_requires=setup_requires,
    cffi_modules=cffi_modules,
    packages=["m3u8", "m3u8.cparser"],
    package_data={
        "m3u8.cparser": ["*.c", "*.h"],
    },
    url="https://github.com/openwurl/m3u8",
    description="Python m3u8 parser",
    long_description=long_description,
    long_description_content_type="text/markdown",
    python_requires=">=3.10",
)
