#include "tests/tests.h"
#include "kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include <stdint.h>

static int passed, failed;

#define CHECK(cond) do {                                          \
        if (cond) passed++;                                       \
        else { failed++; pr_err("mm test failed: %s (line %d)\n", #cond, __LINE__); } \
    } while (0)

#define TEST_VIRT 0xffffd00000000000UL

static void test_pmm(void)
{
    size_t before = pmm_free_count();

    uint64_t a = pmm_alloc(), b = pmm_alloc(), c = pmm_alloc();
    CHECK(a && b && c);
    CHECK(a != b && b != c && a != c);
    CHECK(a % PAGE_SIZE == 0 && b % PAGE_SIZE == 0 && c % PAGE_SIZE == 0);
    CHECK(pmm_free_count() == before - 3);

    memset(phys_to_virt(a), 0xAB, PAGE_SIZE);
    CHECK(((uint8_t *)phys_to_virt(a))[PAGE_SIZE - 1] == 0xAB);

    pmm_free(a);
    pmm_free(b);
    pmm_free(c);
    CHECK(pmm_free_count() == before);

    uint64_t run = pmm_alloc_pages(8);
    CHECK(run && run % PAGE_SIZE == 0);
    CHECK(pmm_free_count() == before - 8);
    pmm_free_pages(run, 8);
    CHECK(pmm_free_count() == before);

    uint64_t z = pmm_alloc_zeroed();
    uint8_t *zp = phys_to_virt(z);
    bool zeroed = true;
    for (size_t i = 0; i < PAGE_SIZE; i++)
        zeroed &= zp[i] == 0;
    CHECK(zeroed);
    pmm_free(z);
}

static void test_vmm(void)
{
    uint64_t phys = pmm_alloc_zeroed();
    CHECK(vmm_map(TEST_VIRT, phys, PTE_WRITE | PTE_NX));

    *(volatile uint64_t *)TEST_VIRT = 0x1122334455667788UL;
    CHECK(*(uint64_t *)phys_to_virt(phys) == 0x1122334455667788UL);

    uint64_t t = 0;
    CHECK(vmm_translate(TEST_VIRT + 0x10, &t) && t == phys + 0x10);

    vmm_unmap(TEST_VIRT);
    CHECK(!vmm_translate(TEST_VIRT, NULL));
    pmm_free(phys);

    // hhdm is mostly 2MiB pages
    CHECK(vmm_translate((uint64_t)phys_to_virt(0x200123), &t) && t == 0x200123);
}

static void test_heap(void)
{
    struct heap_stats s0, s;
    heap_get_stats(&s0);

    enum { N = 64 };
    uint8_t *p[N];
    for (int i = 0; i < N; i++) {
        size_t size = 1 + (size_t)i * 37;
        p[i] = kmalloc(size);
        CHECK(p[i] && (uint64_t)p[i] % 16 == 0);
        memset(p[i], i, size);
    }

    bool intact = true;
    for (int i = 0; i < N; i++)
        for (size_t j = 0; j < 1 + (size_t)i * 37; j++)
            intact &= p[i][j] == (uint8_t)i;
    CHECK(intact);

    // free odd then even so merging happens in both directions
    for (int i = 1; i < N; i += 2)
        kfree(p[i]);
    for (int i = 0; i < N; i += 2)
        kfree(p[i]);

    heap_get_stats(&s);
    CHECK(s.used == s0.used);
    CHECK(s.blocks == s0.blocks);

    // bigger than the initial heap, has to grow
    uint8_t *big = kmalloc(1024 * 1024);
    CHECK(big != NULL);
    if (big) {
        big[0] = 1;
        big[1024 * 1024 - 1] = 2;
        CHECK(big[0] == 1 && big[1024 * 1024 - 1] == 2);
    }
    heap_get_stats(&s);
    CHECK(s.mapped > s0.mapped);
    kfree(big);

    int *z = kcalloc(100, sizeof(int));
    bool zeroed = z != NULL;
    for (int i = 0; z && i < 100; i++)
        zeroed &= z[i] == 0;
    CHECK(zeroed);

    char *r = kmalloc(8);
    strlcpy(r, "abcdefg", 8);
    r = krealloc(r, 4096);
    CHECK(r && strcmp(r, "abcdefg") == 0);
    kfree(r);
    kfree(z);

    CHECK(kmalloc(0) == NULL);
    CHECK(kcalloc(SIZE_MAX / 2, 4) == NULL);

    heap_get_stats(&s);
    CHECK(s.used == s0.used);
}

bool test_mm(void)
{
    passed = failed = 0;
    test_pmm();
    test_vmm();
    test_heap();

    if (failed)
        pr_err("mm: %d/%d tests passed\n", passed, passed + failed);
    else
        pr_ok("mm: all %d tests passed\n", passed);
    return failed == 0;
}
