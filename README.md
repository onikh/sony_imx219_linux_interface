# Sony IMX219 Camera Optimized Linux Interface

Low-level Linux camera pipeline written in C++ and exposed to Python through a native CPython extension module.

Designed for the Sony IMX219 image sensor using the Linux V4L2 subsystem, with performance optimizations including memory-mapped DMA buffers and demosaicing of raw Bayer data in place through a custom processing pipeline.

---

## Tech Stack

- **Languages:** C++, Python
- **Linux APIs:** V4L2, ioctl, mmap
- **Concepts:** DMA buffers, Bayer demosaicing, zero-copy memory access, image processing
