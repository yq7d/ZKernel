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
                              PTE_WRITE | PTE_NX | PTE_NOFREE))
                return ZK_ENOMEM;
        mapped_as = as;
        uint64_t addr = FB_USER_BASE;
        return copy_out(arg, &addr, sizeof addr);
    }
    return ZK_EINVAL;
}

static const struct vnode_ops ops = { .open = fb_open, .close = fb_close, .ioctl = fb_ioctl };
struct vnode fb_vnode = { .type = VNODE_DEV, .refs = 1, .ops = &ops };
