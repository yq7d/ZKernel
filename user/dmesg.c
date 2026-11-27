#include "cat_proc.h"

int main(void)
{
    return cat_file("/proc/kmsg");
}
