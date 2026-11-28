// /dev/fb0: one program at a time maps the framebuffer, the console stops
// drawing until it lets go
#include "drivers/fbdev.h"
#include "arch/x86_64/cpu.h"
#include "console/console.h"
#include "drivers/fb.h"
#include "drivers/tty.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "lib/string.h"

#define FB_USER_BASE 0x0000200000000000UL

static struct file *owner;
static struct address_space *mapped_as;

static uint64_t fb_bytes(void)
{
    return ALIGN_UP((uint64_t)fb.pitch * fb.height, PAGE_SIZE);
}

static int fb_open(struct vnode *v, struct file *f)
{
    (void)v;
    uint64_t flags = irq_save();
    int err = 0;
    if (owner)
        err = ZK_EBUSY;
    else
        owner = f;
    irq_restore(flags);
    if (!err)
        console_suspend(true);
    return err;
}

static void unmap(void)
{
    if (!mapped_as)
        return;
    // only possible from inside the space, it may be gone otherwise
    if (thread_current()->as == mapped_as)
        for (uint64_t off = 0; off < fb_bytes(); off += PAGE_SIZE)
            vmm_unmap_user(mapped_as, FB_USER_BASE + off);
    mapped_as = NULL;
}

static void fb_close(struct vnode *v, struct file *f)
{
    (void)v;
    if (f != owner)
        return;
    unmap();
    owner = NULL;
    console_suspend(false);
}

static int fb_ioctl(struct vnode *v, struct file *f, unsigned long req, uint64_t arg)
{
    (void)v;
    if (req == FBIOGETINFO) {
        struct zk_fbinfo info = {
            .width = fb.width, .height = fb.height, .pitch = fb.pitch, .bpp = 32,
            .red_shift = fb.red_shift, .green_shift = fb.green_shift,
            .blue_shift = fb.blue_shift,
        };
        return copy_out(arg, &info, sizeof info);
    }
    if (req == FBIOMAP) {
        struct address_space *as = thread_current()->as;
        if (f != owner || !as)
            return ZK_EBADF;
        if (mapped_as && mapped_as != as)
            return ZK_EBUSY;
        uint64_t phys = (uint64_t)fb.addr - hhdm_offset;
        for (uint64_t off = 0; off < fb_bytes(); off += PAGE_SIZE)
            if (!vmm_map_user(as, FB_USER_BASE + off, phys + off,
                              PTE_WRITE | PTE_NX | PTE_NOFREE | PTE_WC))
                return ZK_ENOMEM;
        mapped_as = as;
        uint64_t addr = FB_USER_BASE;
        return copy_out(arg, &addr, sizeof addr);
    }
    return ZK_EINVAL;
}

// linux's fbdev interface
struct fb_bitfield {
    uint32_t offset, length, msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};

struct fb_fix_screeninfo {
    char id[16];
    uint64_t smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities, reserved[2];
};

_Static_assert(sizeof(struct fb_var_screeninfo) == 160, "fb_var_screeninfo");
_Static_assert(sizeof(struct fb_fix_screeninfo) == 80, "fb_fix_screeninfo");

static void var_info(struct fb_var_screeninfo *v)
{
    memset(v, 0, sizeof *v);
    v->xres = v->xres_virtual = fb.width;
    v->yres = v->yres_virtual = fb.height;
    v->bits_per_pixel = 32;
    v->red = (struct fb_bitfield){ fb.red_shift, 8, 0 };
    v->green = (struct fb_bitfield){ fb.green_shift, 8, 0 };
    v->blue = (struct fb_bitfield){ fb.blue_shift, 8, 0 };
    v->height = v->width = 0xffffffff;    // size in mm unknown
    v->pixclock = 10000;
    v->vmode = 0;                         // FB_VMODE_NONINTERLACED
}

static int fb_lioctl(struct vnode *v, struct file *f, unsigned long req, uint64_t arg)
{
    (void)v; (void)f;
    switch (req) {
    case 0x4600:    // FBIOGET_VSCREENINFO
    case 0x4601: {  // FBIOPUT_VSCREENINFO, the mode can't change: hand back the one there is
        struct fb_var_screeninfo var;
        var_info(&var);
        return copy_out(arg, &var, sizeof var);
    }
    case 0x4602: {  // FBIOGET_FSCREENINFO
        struct fb_fix_screeninfo fix;
        memset(&fix, 0, sizeof fix);
        memcpy(fix.id, "zkfb", 5);
        fix.smem_start = (uint64_t)fb.addr - hhdm_offset;
        fix.smem_len = (uint32_t)fb_bytes();
        fix.type = 0;      // FB_TYPE_PACKED_PIXELS
        fix.visual = 2;    // FB_VISUAL_TRUECOLOR
        fix.line_length = fb.pitch;
        return copy_out(arg, &fix, sizeof fix);
    }
    case 0x4604:    // FBIOGETCMAP
    case 0x4605:    // FBIOPUTCMAP
    case 0x4606:    // FBIOPAN_DISPLAY
    case 0x4611:    // FBIOBLANK
    case 0x4620:    // FBIO_WAITFORVSYNC
        return 0;
    }
    return ZK_EINVAL;
}

static int fb_mmap(struct vnode *v, struct file *f, uint64_t va, uint64_t len,
                   uint64_t off, uint64_t pte_flags)
{
    (void)v; (void)f;
    struct address_space *as = thread_current()->as;
    if (off + len > fb_bytes() || (off & (PAGE_SIZE - 1)))
        return ZK_EINVAL;
    uint64_t phys = (uint64_t)fb.addr - hhdm_offset + off;
    for (uint64_t o = 0; o < len; o += PAGE_SIZE)
        if (!vmm_map_user(as, va + o, phys + o, pte_flags | PTE_NOFREE | PTE_WC))
            return ZK_ENOMEM;
    return 0;
}

static const struct vnode_ops ops = {
    .open = fb_open, .close = fb_close, .ioctl = fb_ioctl,
    .lioctl = fb_lioctl, .mmap = fb_mmap,
};
struct vnode fb_vnode = { .type = VNODE_DEV, .refs = 1, .ops = &ops };
