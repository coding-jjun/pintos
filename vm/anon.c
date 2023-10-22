/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/vaddr.h"
#include "bitmap.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

struct bitmap *swap_table;

const size_t SECTORS_PER_PAGE = PGSIZE / DISK_SECTOR_SIZE;

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1, 1);
	int swap_table_size = disk_size(swap_disk) / SECTORS_PER_PAGE;
	swap_table = bitmap_create(swap_table_size);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;
	
	struct anon_page *anon_page = &page->anon;
	anon_page->swap_index = -1;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	int swap_index = anon_page->swap_index;

	if (bitmap_test(swap_table, swap_index) == false) {
		return false;
	}

	for (int i = 0; i < SECTORS_PER_PAGE; ++i) {
		disk_read(swap_disk, i + SECTORS_PER_PAGE * swap_index, kva + i * DISK_SECTOR_SIZE);
	}

	bitmap_set(swap_table, anon_page->swap_index, false);

	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	int swap_index = bitmap_scan(swap_table, 0, 1, false);
	if (swap_index == BITMAP_ERROR) {
		return false;
	}

	for(int i = 0; i < SECTORS_PER_PAGE; ++i) {
		disk_write(swap_disk, i + SECTORS_PER_PAGE * swap_index, (page->va) + i * DISK_SECTOR_SIZE);
	}

	bitmap_set(swap_table, swap_index, true);
	pml4_clear_page(thread_current()->pml4, page->va);

	anon_page->swap_index = swap_index;
	return true;
}
/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	pml4_clear_page(thread_current()->pml4, page->va);

}
