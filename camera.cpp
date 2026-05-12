#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <linux/v4l2-subdev.h>

// define image paramaters and buffer count
#define BUFFER_COUNT 4
#define WIDTH        640
#define HEIGHT       480

//set file descriptors to uninitialized
static int      g_fd        = -1;          // /dev/video0
static int      g_subdev_fd = -1;          // /dev/v4l-subdev0
static uint8_t *g_rgb       = NULL;        // pointer to our output array
static unsigned g_buf_count = 0; // set to zero in case not enough space to allocate for buffers, change after we confirm there's space

struct FrameBuffer {
    void   *start;
    size_t  length;
}; 

typedef struct FrameBuffer FrameBuffer_t;

static FrameBuffer_t g_buffers[BUFFER_COUNT];

//helper function to set camera parameters 
static int set_ctrl(int fd, uint32_t cid, int32_t value) {
    struct v4l2_ext_control  ctrl  = {0};
    struct v4l2_ext_controls ctrls = {0};
    ctrl.id       = cid;
    ctrl.value    = value;
    ctrls.which   = V4L2_CTRL_WHICH_CUR_VAL;
    ctrls.count   = 1;
    ctrls.controls = &ctrl;
    return ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls);
}


//return a pixel from the raw bayer data
static inline uint16_t px(const uint16_t *raw, int x, int y) {
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x >= WIDTH) x = WIDTH-1; if (y >= HEIGHT) y = HEIGHT-1;
    return raw[y * WIDTH + x];
}



//convert from bayer to rgb
static void demosaic_rggb(const uint16_t *raw, uint8_t *rgb) {

    //Iterate by twos bc 2X2 Bayer pixels is smallest unit of useful information

    for (int y = 0; y < HEIGHT; y += 2) {

        const uint16_t* r1 = &raw[y * WIDTH]; //Red, green
        const uint16_t* r2 = &raw[(y + 1) * WIDTH]; //Green, blue
        
        // Output corresponding to Bayer input
        uint8_t* out1 = &rgb[y * WIDTH * 3];
        uint8_t* out2 = &rgb[(y + 1) * WIDTH * 3];

        for (int x = 0; x < WIDTH; x += 2) {
            // Bayer rggb:
            // r1[x] -> red,   r1[x+1] ->green
            // r2[x] -> green, r2[x+1] -> blue

            // Pre-shift the raw values once
            uint8_t R = (uint8_t)(r1[x] >> 2);
            uint8_t G = (uint8_t)(r1[x+1] >> 2); 
            uint8_t B = (uint8_t)(r2[x+1] >> 2);
            //discard the other green to save time

            // write pixel (y, x)
            out1[0] = R; out1[1] = G; out1[2] = B;
            // write pixel (y, x+1)
            out1[3] = R; out1[4] = G; out1[5] = B;
            // write pixel (y+1, x)
            out2[0] = R; out2[1] = G; out2[2] = B;
            // write pixel (y+1, x+1)
            out2[3] = R; out2[4] = G; out2[5] = B;

            out1 += 6; // move output pointers forward by 2 pixels bc 2 * 3 bytes
            out2 += 6;
        }
    }
}



//Set up camera object and pipeline
static PyObject* camera_open(PyObject *self, PyObject *args) {

    if (g_fd >= 0) {
        PyErr_SetString(PyExc_RuntimeError, "camera already open");
        return NULL;
    } //anything that looks like this is error handling based on file descriptor values or ioctl outputs

    // open subdev for configutation
    g_subdev_fd = open("/dev/v4l-subdev0", O_RDWR);
    if (g_subdev_fd < 0) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, "/dev/v4l-subdev0");
        return NULL;
    }

    // set subdev parameters
    struct v4l2_subdev_format subdev_fmt = {0};
    subdev_fmt.which              = V4L2_SUBDEV_FORMAT_ACTIVE;
    subdev_fmt.pad                = 0;
    subdev_fmt.format.width       = WIDTH;
    subdev_fmt.format.height      = HEIGHT;
    subdev_fmt.format.code        = 0x300f;   // MEDIA_BUS_FMT_SRGGB10_1X10
    subdev_fmt.format.field       = V4L2_FIELD_NONE;
    subdev_fmt.format.colorspace  = V4L2_COLORSPACE_RAW;

    if (ioctl(g_subdev_fd, VIDIOC_SUBDEV_S_FMT, &subdev_fmt) < 0) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, "VIDIOC_SUBDEV_S_FMT");
        close(g_subdev_fd); g_subdev_fd = -1;
        return NULL;
    }

    // set specific controls
    set_ctrl(g_subdev_fd, V4L2_CID_EXPOSURE,      2500);
    set_ctrl(g_subdev_fd, V4L2_CID_ANALOGUE_GAIN,  600);
    set_ctrl(g_subdev_fd, V4L2_CID_DIGITAL_GAIN,   512);
    // White balance — use the values you tuned in stage 2
    set_ctrl(g_subdev_fd, 0x009e0904, 1023);  // red
    set_ctrl(g_subdev_fd, 0x009e0905,  450);  // green-red
    set_ctrl(g_subdev_fd, 0x009e0906,  900);  // blue
    set_ctrl(g_subdev_fd, 0x009e0907,  450);  // green-blue

    // open dev (camera itself)
    g_fd = open("/dev/video0", O_RDWR);
    if (g_fd < 0) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, "/dev/video0");
        close(g_subdev_fd); g_subdev_fd = -1;
        return NULL;
    }

    //set parameters to match subdev exactly
    struct v4l2_format fmt = {0};
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = WIDTH;
    fmt.fmt.pix.height      = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_SRGGB10;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if (ioctl(g_fd, VIDIOC_S_FMT, &fmt) < 0) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, "VIDIOC_S_FMT");
        close(g_fd); g_fd = -1;
        close(g_subdev_fd); g_subdev_fd = -1;
        return NULL;
    }

    // allocate kernel buffers
    struct v4l2_requestbuffers req = {0};
    req.count  = BUFFER_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(g_fd, VIDIOC_REQBUFS, &req) < 0) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, "VIDIOC_REQBUFS");
        close(g_fd); g_fd = -1;
        close(g_subdev_fd); g_subdev_fd = -1;
        return NULL;
    }
    g_buf_count = req.count;

    for (unsigned i = 0; i < g_buf_count; i++) {
        // in this loop, set up our frame buffer structs array
        struct v4l2_buffer buf = {0};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(g_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            PyErr_SetFromErrnoWithFilename(PyExc_OSError, "VIDIOC_QUERYBUF");
            close(g_fd); g_fd = -1;
            return NULL;
        }
        g_buffers[i].length = buf.length;
        g_buffers[i].start  = mmap(NULL, buf.length,
                                   PROT_READ|PROT_WRITE, MAP_SHARED,
                                   g_fd, buf.m.offset); //get pointer to dma buffer
        if (g_buffers[i].start == MAP_FAILED) {
            PyErr_SetFromErrno(PyExc_OSError);
            close(g_fd); g_fd = -1;
            return NULL;
        }
    }

    // this is output buffer
    g_rgb = (uint8_t *)malloc(WIDTH * HEIGHT * 3);
    if (!g_rgb) {
        PyErr_NoMemory();
        close(g_fd); g_fd = -1;
        return NULL;
    }

    // queue buffers to prepare for streaming
    for (unsigned i = 0; i < g_buf_count; i++) {
        struct v4l2_buffer buf = {0};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(g_fd, VIDIOC_QBUF, &buf) < 0) {
            PyErr_SetFromErrnoWithFilename(PyExc_OSError, "VIDIOC_QBUF");
            return NULL;
        }
    }

    // start data stream
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(g_fd, VIDIOC_STREAMON, &type) < 0) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, "VIDIOC_STREAMON");
        return NULL;
    }

    Py_RETURN_NONE;
}

// capture frame
static PyObject* camera_capture(PyObject *self, PyObject *args) {

    if (g_fd < 0) {
        PyErr_SetString(PyExc_RuntimeError, "camera not open — call camera.open() first");
        return NULL;
    }

    // block until kernel has a completed frame, then dequeue
    struct v4l2_buffer buf = {0};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(g_fd, VIDIOC_DQBUF, &buf) < 0) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, "VIDIOC_DQBUF");
        return NULL;
    }

    // convert dequeued buffer into rgb
    demosaic_rggb((const uint16_t *)g_buffers[buf.index].start, g_rgb);

    // queue it again
    if (ioctl(g_fd, VIDIOC_QBUF, &buf) < 0) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, "VIDIOC_QBUF (requeue)");
        return NULL;
    }

    // Send to python
    return PyMemoryView_FromMemory(
        (char *)g_rgb,
        WIDTH * HEIGHT * 3,
        PyBUF_READ
    );
}

//shutoff camera stream
static PyObject* camera_close(PyObject *self, PyObject *args) {
    if (g_fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(g_fd, VIDIOC_STREAMOFF, &type);
        for (unsigned i = 0; i < g_buf_count; i++)
            munmap(g_buffers[i].start, g_buffers[i].length);
        close(g_fd);
        g_fd = -1;
    }
    if (g_subdev_fd >= 0) { close(g_subdev_fd); g_subdev_fd = -1; }
    if (g_rgb) { free(g_rgb); g_rgb = NULL; }
    Py_RETURN_NONE;
}

//Define methods
static PyMethodDef CameraMethods[] = {
    {"open",    camera_open,    METH_NOARGS, "Open and start the camera pipeline"},
    {"capture", camera_capture, METH_NOARGS, "Capture one frame, return memoryview"},
    {"close",   camera_close,   METH_NOARGS, "Stop streaming and release resources"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef CameraModule = {
    PyModuleDef_HEAD_INIT, "camera", NULL, -1, CameraMethods
};

PyMODINIT_FUNC PyInit_camera(void) {
    return PyModule_Create(&CameraModule);
}
