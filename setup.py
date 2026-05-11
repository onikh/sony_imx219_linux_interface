from setuptools import setup, Extension

setup(
    name="camera",
    ext_modules=[
        Extension(
            "camera",
            sources=["camera.cpp"],
            extra_compile_args=[
    "-O3",
    "-march=armv6zk",       # This is the winner for Pi Zero W
    "-mcpu=arm1176jzf-s",   # This tells it the specific core
    "-mfpu=vfp",
    "-mfloat-abi=hard",
    "-ffast-math",
],
        )
    ],
)
