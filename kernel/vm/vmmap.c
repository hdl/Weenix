#include "kernel.h"
#include "errno.h"
#include "globals.h"

#include "vm/vmmap.h"
#include "vm/shadow.h"
#include "vm/anon.h"

#include "proc/proc.h"

#include "util/debug.h"
#include "util/list.h"
#include "util/string.h"
#include "util/printf.h"

#include "fs/vnode.h"
#include "fs/file.h"
#include "fs/fcntl.h"
#include "fs/vfs_syscall.h"

#include "mm/slab.h"
#include "mm/page.h"
#include "mm/mm.h"
#include "mm/mman.h"
#include "mm/mmobj.h"

#define USER_PAGE_HIGH (USER_MEM_HIGH - USER_MEM_LOW) / PAGE_SIZE
#define VMMAP_FLAG 1
#define dprintf(...)                    \
        if (VMMAP_FLAG) {               \
            dbg(DBG_VMMAP, __VA_ARGS__);\
        }
/*my own macro*/

static void
print_vmmap(vmmap_t *vmmap)
{
    if (VMMAP_FLAG) {
        dbginfo(DBG_VMMAP, vmmap_mapping_info, vmmap);
    }
}

static void
print_vmarea(vmarea_t *vmarea)
{
    dprintf("The vmarea is: [%u, %u), offset is %u\n", vmarea->vma_start, vmarea->vma_end, vmarea->vma_off);
}

static int
valid_pagenumber(uint32_t pagenum)
{
    return (pagenum < USER_PAGE_HIGH);
}

static slab_allocator_t *vmmap_allocator;
static slab_allocator_t *vmarea_allocator;

void
vmmap_init(void)
{
        vmmap_allocator = slab_allocator_create("vmmap", sizeof(vmmap_t));
        KASSERT(NULL != vmmap_allocator && "failed to create vmmap allocator!");
        vmarea_allocator = slab_allocator_create("vmarea", sizeof(vmarea_t));
        KASSERT(NULL != vmarea_allocator && "failed to create vmarea allocator!");
}

vmarea_t *
vmarea_alloc(void)
{
        vmarea_t *newvma = (vmarea_t *) slab_obj_alloc(vmarea_allocator);
        if (newvma) {
                newvma->vma_vmmap = NULL;
        }
        return newvma;
}

void
vmarea_free(vmarea_t *vma)
{
        KASSERT(NULL != vma);
        slab_obj_free(vmarea_allocator, vma);
}

/* Create a new vmmap, which has no vmareas and does
 * not refer to a process. */
vmmap_t *
vmmap_create(void)
{
    vmmap_t *newvmm = (vmmap_t *)slab_obj_alloc(vmmap_allocator);
    if (newvmm) {
        list_init(&newvmm->vmm_list);
        newvmm->vmm_proc = NULL;
        KASSERT(list_empty(&newvmm->vmm_list));
    }
    return newvmm;
        /*NOT_YET_IMPLEMENTED("VM: vmmap_create");*/
        /*return NULL;*/
}

/* Removes all vmareas from the address space and frees the
 * vmmap struct. */
void
vmmap_destroy(vmmap_t *map)
{
    KASSERT(map);

    /*vmarea_t pointer*/
    vmarea_t *vma;

    /*traversal thru vmm_list and remove it*/
    list_iterate_begin(&map->vmm_list, vma, vmarea_t, vma_plink) {
        /*since there is 1 less pointer pointing to vma_obj, put it*/
        vma->vma_obj->mmo_ops->put(vma->vma_obj);
        /*remove it from the list*/
        list_remove(&vma->vma_plink);
        /*reclaim the memory*/
        vmarea_free(vma);
    } list_iterate_end();
    
    /*Any other things than just remove the link?*/

    slab_obj_free(vmmap_allocator, map);
        /*NOT_YET_IMPLEMENTED("VM: vmmap_destroy");*/
}

/* Add a vmarea to an address space. Assumes (i.e. asserts to some extent)
 * the vmarea is valid.  This involves finding where to put it in the list
 * of VM areas, and adding it. Don't forget to set the vma_vmmap for the
 * area. */
void
vmmap_insert(vmmap_t *map, vmarea_t *newvma)
{
    KASSERT(map);

    KASSERT(newvma);
    /*sanity check for newvma*/
    KASSERT(newvma->vma_end > newvma->vma_start);

    dprintf("vmmap_insert is called:\n");
    dprintf("before inserting, the vmmap is:\n");
    print_vmmap(map);
    dprintf("the vmarea to be inserted is:\n");
    print_vmarea(newvma);

    if list_empty(&map->vmm_list) {
        list_insert_head(&map->vmm_list, &newvma->vma_plink);
        newvma->vma_vmmap = map;
        return;
    }

    list_link_t *list = &map->vmm_list;
    vmarea_t *vma_cur;
    list_iterate_begin(list, vma_cur, vmarea_t, vma_plink) {
        if (vma_cur->vma_plink.l_prev == list) {
            /*no prev*/
            if (newvma->vma_end <= vma_cur->vma_start) {
                list_insert_head(list, &newvma->vma_plink);
                newvma->vma_vmmap = map;
                return;
            }
        } else {
            /*prev is a vmarea*/
            vmarea_t *vma_prev = list_item(vma_cur->vma_plink.l_prev, vmarea_t, vma_plink);
            KASSERT(vma_prev);
            if (newvma->vma_end <= vma_cur->vma_start && newvma->vma_start >= vma_prev->vma_end) {
                list_insert_before(&vma_cur->vma_plink, &newvma->vma_plink);
                newvma->vma_vmmap = map;
                return;
            }
        }
    } list_iterate_end();

    KASSERT(vma_cur->vma_plink.l_next == list);
    list_insert_tail(list, &newvma->vma_plink);
    newvma->vma_vmmap = map;

    dprintf("after inserting, the vmmap is:\n");
    print_vmmap(map);
    dprintf("the vmarea inserted is:\n");
    print_vmarea(newvma);
        /*NOT_YET_IMPLEMENTED("VM: vmmap_insert");*/
}

/* Find a contiguous range of free virtual pages of length npages in
 * the given address space. Returns starting vfn for the range,
 * without altering the map. Returns -1 if no such range exists.
 *
 * Your algorithm should be first fit. If dir is VMMAP_DIR_HILO, you
 * should find a gap as high in the address space as possible; if dir
 * is VMMAP_DIR_LOHI, the gap should be as low as possible. */
int
vmmap_find_range(vmmap_t *map, uint32_t npages, int dir)
{
    KASSERT(map);

    dprintf("find_range, vmmap is:\n");
    print_vmmap(map);
    dprintf("looking for %u pages\n", npages);

    /*low-high*/
    if (dir == VMMAP_DIR_LOHI || dir == 0) {
        if list_empty(&map->vmm_list) {
            return 0;
        }

        list_link_t *list = &map->vmm_list;
        vmarea_t *vma_cur;
        list_iterate_begin(list, vma_cur, vmarea_t, vma_plink) {
            if (vma_cur->vma_plink.l_prev == list) {
                /*no prev*/
                /*head of list*/
                if (vma_cur->vma_start >= npages) {
                    return 0;
                }
            } else {
                /*prev is a vmarea*/
                vmarea_t *vma_prev = list_item(vma_cur->vma_plink.l_prev, vmarea_t, vma_plink);
                KASSERT(vma_prev);
                if (vma_cur->vma_start - vma_prev->vma_end >= npages) {
                    return vma_prev->vma_end;
                }
            }
        } list_iterate_end();

        KASSERT(vma_cur->vma_plink.l_next == list);
        if (USER_PAGE_HIGH - vma_cur->vma_end >= npages) {
            return vma_cur->vma_end;
        } else {
            return -1;
        }
    } else {
        /*high-low*/
        if list_empty(&map->vmm_list) {
            return (USER_PAGE_HIGH - npages);
        }

        list_link_t *list = &map->vmm_list;
        vmarea_t *vma_cur;
        list_iterate_reverse(list, vma_cur, vmarea_t, vma_plink) {
            if (vma_cur->vma_plink.l_next == list) {
                /*no prev*/
                /*head of list*/
                if (USER_PAGE_HIGH - vma_cur->vma_end >= npages) {
                    return vma_cur->vma_end;
                }
            } else {
                /*next is a vmarea*/
                vmarea_t *vma_next = list_item(vma_cur->vma_plink.l_next, vmarea_t, vma_plink);
                KASSERT(vma_next);
                if (vma_next->vma_start - vma_cur->vma_end >= npages) {
                    return vma_cur->vma_end;
                }
            }
        } list_iterate_end();

        KASSERT(vma_cur->vma_plink.l_prev == list);
        if (vma_cur->vma_start >= npages) {
            return 0;
        } else {
            return -1;
        }
    }
        /*NOT_YET_IMPLEMENTED("VM: vmmap_find_range");*/
        /*return -1;*/
}

/* Find the vm_area that vfn lies in. Simply scan the address space
 * looking for a vma whose range covers vfn. If the page is unmapped,
 * return NULL. */
vmarea_t *
vmmap_lookup(vmmap_t *map, uint32_t vfn)
{
    KASSERT(map);
    /*assumption here is that the vfn is already auditted by some other routine*/
    KASSERT(valid_pagenumber(vfn));
    dprintf("vmmap_lookup, vfn is %u\n", vfn);
    print_vmmap(map);

    vmarea_t *vma;

    list_iterate_begin(&map->vmm_list, vma, vmarea_t, vma_plink) {
        if (vfn >= vma->vma_start && vfn < vma->vma_end) {
            return vma;
        }
    } list_iterate_end();

    return NULL;
        /*NOT_YET_IMPLEMENTED("VM: vmmap_lookup");*/
        /*return NULL;*/
}

/* Allocates a new vmmap containing a new vmarea for each area in the
 * given map. The areas should have no mmobjs set yet. Returns pointer
 * to the new vmmap on success, NULL on failure. This function is
 * called when implementing fork(2). */
vmmap_t *
vmmap_clone(vmmap_t *map)
{
        NOT_YET_IMPLEMENTED("VM: vmmap_clone");
        return NULL;
}

/* Insert a mapping into the map starting at lopage for npages pages.
 * If lopage is zero, we will find a range of virtual addresses in the
 * process that is big enough, by using vmmap_find_range with the same
 * dir argument.  If lopage is non-zero and the specified region
 * contains another mapping that mapping should be unmapped.
 *
 * If file is NULL an anon mmobj will be used to create a mapping
 * of 0's.  If file is non-null that vnode's file will be mapped in
 * for the given range.  Use the vnode's mmap operation to get the
 * mmobj for the file; do not assume it is file->vn_obj. Make sure all
 * of the area's fields except for vma_obj have been set before
 * calling mmap.
 *
 * If MAP_PRIVATE is specified set up a shadow object for the mmobj.
 *
 * All of the input to this function should be valid (KASSERT!).
 * See mmap(2) for for description of legal input.
 * Note that off should be page aligned.
 *
 * Be very careful about the order operations are performed in here. Some
 * operation are impossible to undo and should be saved until there
 * is no chance of failure.
 *
 * If 'new' is non-NULL a pointer to the new vmarea_t should be stored in it.
 */
int
vmmap_map(vmmap_t *map, vnode_t *file, uint32_t lopage, uint32_t npages,
          int prot, int flags, off_t off, int dir, vmarea_t **new)
{
    KASSERT(map);
    KASSERT(PAGE_ALIGNED(off));

    vmarea_t *vma_result = vmarea_alloc();
    if (vma_result == NULL) {
        return -ENOSPC;
    }

    int remove = 0;

    if (lopage == 0) {
        int ret = vmmap_find_range(map, npages, dir);
        if (ret < 0) {
            KASSERT(ret == -1);
            /*not sure about the return value*/
            return -1;
        }
        dprintf("the range found is: [%d, %d)\n", ret, ret + npages);
        lopage = (unsigned)ret;
    } else {
        /*vmmap_remove(map, lopage, npages);*/
        remove = 1;
    }
    uint32_t hipage = lopage + npages;

    vma_result->vma_start = lopage;
    vma_result->vma_end = lopage + npages;
    vma_result->vma_off = 0; /*what about vma_off?*/

    vma_result->vma_prot = prot;
    vma_result->vma_flags = flags;

    /*vmmap_insert(map, vma_result); [>also take care of vma_plink<]*/

    /*vma_obj need not to be set*/
    /*vma_olink is initialized during alloc, still unclear, what to do?*/

    if (file == NULL) {
        mmobj_t *mmobj_anon = anon_create();
        if (mmobj_anon == NULL) {
            return -ENOSPC;
        }

        uint32_t pagenum = lopage;
        for (pagenum = lopage ; pagenum < hipage ; pagenum++) {
            int err;
            pframe_t *pf;
            /*lookup(alloc) the page*/
            /*lookuppage will also fill the page to 0s*/
            err = mmobj_anon->mmo_ops->lookuppage(mmobj_anon, pagenum, 1, &pf);
            if (err < 0) {
                KASSERT(pf == NULL);
                return err;
            }
        }

        /*hook it up with the virtual memory area*/
        /*not quite sure*/
        vma_result->vma_obj = mmobj_anon;
    } else {
        /*calling mmap*/
        /*TODO: take care of off*/
        mmobj_t *mmobj_ret;
        int err = file->vn_ops->mmap(file, vma_result, &mmobj_ret);
        if (err < 0) {
            KASSERT(mmobj_ret == NULL);
            return err;
        }
    }

    if (flags & MAP_PRIVATE) {
        /*create a shadow object*/
    }

    /*time for the final move*/
    if (remove) {
        vmmap_remove(map, lopage, npages);
    }

    vmmap_insert(map, vma_result);
    return 0;
        /*NOT_YET_IMPLEMENTED("VM: vmmap_map");*/
        /*return -1;*/
}

/*
 * We have no guarantee that the region of the address space being
 * unmapped will play nicely with our list of vmareas.
 *
 * You must iterate over each vmarea that is partially or wholly covered
 * by the address range [addr ... addr+len). The vm-area will fall into one
 * of four cases, as illustrated below:
 *
 * key:
 *          [             ]   Existing VM Area
 *        *******             Region to be unmapped
 *
 * Case 1:  [   ******    ]
 * The region to be unmapped lies completely inside the vmarea. We need to
 * split the old vmarea into two vmareas. be sure to increment the
 * reference count to the file associated with the vmarea.
 *
 * Case 2:  [      *******]**
 * The region overlaps the end of the vmarea. Just shorten the length of
 * the mapping.
 *
 * Case 3: *[*****        ]
 * The region overlaps the beginning of the vmarea. Move the beginning of
 * the mapping (remember to update vma_off), and shorten its length.
 *
 * Case 4: *[*************]**
 * The region completely contains the vmarea. Remove the vmarea from the
 * list.
 */
int
vmmap_remove(vmmap_t *map, uint32_t lopage, uint32_t npages)
{
    KASSERT(map);
    /*KASSERT(!list_empty(&map->vmm_list));*/

    dprintf("before vmmap_remove:\n");
    print_vmmap(map);
    dprintf("the range is [%u, %u)\n", lopage, lopage + npages);

    vmarea_t *vma;
    uint32_t hipage = lopage + npages;

    list_iterate_begin(&map->vmm_list, vma, vmarea_t, vma_plink) {
        /*case 1*/
        /*split into 2 vmareas*/
        /*[vma_start, lopage) and [hipage, vma_end)*/
        if (vma->vma_start < lopage && vma->vma_end > hipage) {
            vmarea_t *vma_new = vmarea_alloc();
            if (vma_new == NULL) {
                return -ENOSPC;
            }
            vma_new->vma_start = vma->vma_start;
            vma_new->vma_end = lopage;
            vma_new->vma_off = vma->vma_off;

            vma_new->vma_prot = vma->vma_prot;
            vma_new->vma_flags = vma->vma_flags;

            vma_new->vma_vmmap = vma->vma_vmmap;
            vma_new->vma_obj = vma->vma_obj;
            vma_new->vma_obj->mmo_ops->ref(vma_new->vma_obj);

            list_init(&vma_new->vma_plink);
            list_insert_before(&vma->vma_plink, &vma_new->vma_plink);
            list_init(&vma_new->vma_olink);
            /*not sure about what I could do here with olink*/

            vma->vma_off = hipage - vma->vma_start + vma->vma_off;
            vma->vma_start = hipage;
        }

        /*case 2*/
        /*chop off the right part*/
        if (vma->vma_start < lopage && vma->vma_end <= hipage) {
            vma->vma_end = lopage;
        }

        /*case 3*/
        /*chop off the left part*/
        if (vma->vma_start >= lopage && vma->vma_end > hipage) {
            vma->vma_off = hipage - vma->vma_start + vma->vma_off;
            vma->vma_start = hipage;
        }

        /*case 4*/
        /*just remove it*/
        if (vma->vma_start >= lopage && vma->vma_end <= hipage) {
            vma->vma_obj->mmo_ops->put(vma->vma_obj);
            list_remove(&vma->vma_plink);
            /*not sure about removing it*/
            list_remove(&vma->vma_olink);
            vmarea_free(vma);
        }
    } list_iterate_end();
    
    dprintf("after vmmap_remove:\n");
    print_vmmap(map);

    return 0;
        /*NOT_YET_IMPLEMENTED("VM: vmmap_remove");*/
        /*return -1;*/
}

/*
 * Returns 1 if the given address space has no mappings for the
 * given range, 0 otherwise.
 */
int
vmmap_is_range_empty(vmmap_t *map, uint32_t startvfn, uint32_t npages)
{
    KASSERT(map);

    dprintf("determine if the range [%u, %u) is empty in map:\n", startvfn, startvfn + npages);
    print_vmmap(map);

    vmarea_t *vma;
    list_iterate_begin(&map->vmm_list, vma, vmarea_t, vma_plink) {
        if ((vma->vma_start >= startvfn + npages) || (vma->vma_end <= startvfn)) {
            /*nothing*/
        } else {
            dprintf("Hmm, not empty, found some overlapping\n");
            return 0;
        }
    } list_iterate_end();

    dprintf("turns out it's emtpy for that range\n");
    return 1;
        /*NOT_YET_IMPLEMENTED("VM: vmmap_is_range_empty");*/
        /*return 0;*/
}

/* Read into 'buf' from the virtual address space of 'map' starting at
 * 'vaddr' for size 'count'. To do so, you will want to find the vmareas
 * to read from, then find the pframes within those vmareas corresponding
 * to the virtual addresses you want to read, and then read from the
 * physical memory that pframe points to. You should not check permissions
 * of the areas. Assume (KASSERT) that all the areas you are accessing exist.
 * Returns 0 on success, -errno on error.
 */
int
vmmap_read(vmmap_t *map, const void *vaddr, void *buf, size_t count)
{
        NOT_YET_IMPLEMENTED("VM: vmmap_read");
        return 0;
}

/* Write from 'buf' into the virtual address space of 'map' starting at
 * 'vaddr' for size 'count'. To do this, you will need to find the correct
 * vmareas to write into, then find the correct pframes within those vmareas,
 * and finally write into the physical addresses that those pframes correspond
 * to. You should not check permissions of the areas you use. Assume (KASSERT)
 * that all the areas you are accessing exist. Remember to dirty pages!
 * Returns 0 on success, -errno on error.
 */
int
vmmap_write(vmmap_t *map, void *vaddr, const void *buf, size_t count)
{
        NOT_YET_IMPLEMENTED("VM: vmmap_write");
        return 0;
}

/* a debugging routine: dumps the mappings of the given address space. */
size_t
vmmap_mapping_info(const void *vmmap, char *buf, size_t osize)
{
        KASSERT(0 < osize);
        KASSERT(NULL != buf);
        KASSERT(NULL != vmmap);

        vmmap_t *map = (vmmap_t *)vmmap;
        vmarea_t *vma;
        ssize_t size = (ssize_t)osize;

        int len = snprintf(buf, size, "%21s %5s %7s %8s %10s %12s\n",
                           "VADDR RANGE", "PROT", "FLAGS", "MMOBJ", "OFFSET",
                           "VFN RANGE");

        list_iterate_begin(&map->vmm_list, vma, vmarea_t, vma_plink) {
                size -= len;
                buf += len;
                if (0 >= size) {
                        goto end;
                }

                len = snprintf(buf, size,
                               "%#.8x-%#.8x  %c%c%c  %7s 0x%p %#.5x %#.5x-%#.5x\n",
                               vma->vma_start << PAGE_SHIFT,
                               vma->vma_end << PAGE_SHIFT,
                               (vma->vma_prot & PROT_READ ? 'r' : '-'),
                               (vma->vma_prot & PROT_WRITE ? 'w' : '-'),
                               (vma->vma_prot & PROT_EXEC ? 'x' : '-'),
                               (vma->vma_flags & MAP_SHARED ? " SHARED" : "PRIVATE"),
                               vma->vma_obj, vma->vma_off, vma->vma_start, vma->vma_end);
        } list_iterate_end();

end:
        if (size <= 0) {
                size = osize;
                buf[osize - 1] = '\0';
        }
        /*
        KASSERT(0 <= size);
        if (0 == size) {
                size++;
                buf--;
                buf[0] = '\0';
        }
        */
        return osize - size;
}
