/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "userprog/process.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;
	
	struct file_page *file_page = &page->file;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset) {
	file_reopen(file);
	
	size_t read_bytes = length > file_length(file) ? file_length(file) : length;
	size_t zero_bytes = PGSIZE - read_bytes % PGSIZE;
	
	if (!load_segment(file, offset, addr, read_bytes, zero_bytes, writable, VM_FILE)) {
		return NULL;
	}
	return addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
//   struct thread *cur = thread_current();
//   struct file *file;
//   struct page *first_page = spt_find_page(&cur->spt, addr);
//   if (first_page == NULL)
//     return;

//   file = ((struct lazy_load_info *)first_page->uninit.aux)->file;

//   while (true) {
//     struct page *page = spt_find_page(&cur->spt, addr);
//     if (page == NULL)
//       break;

//     struct lazy_load_info *aux = (struct lazy_load_info *)page->uninit.aux;

//     if (file != aux->file)
//       break;

//     if (page->operations->type == VM_UNINIT) {
//       free(page->uninit.aux);
//     }
//     if (pml4_is_dirty(cur->pml4, page->va)) {
//       lock_acquire(inode_get_lock(file_get_inode(aux->file)));
//       file_write_at(aux->file, addr, aux->read_bytes, aux->ofs);
//       lock_release(inode_get_lock(file_get_inode(aux->file)));

//       pml4_set_dirty(cur->pml4, page->va, 0);
//     }

//     pml4_clear_page(cur->pml4, page->va);
//     addr += PGSIZE;
//   }
    while(true){
        struct page* page = spt_find_page(&thread_current()->spt, addr);

        if(page == NULL)
            break;

        struct lazy_load_info *aux = (struct lazy_load_info *)page->uninit.aux;

        // dirty check
        if(pml4_is_dirty(thread_current()->pml4, page->va)){
            file_write_at(aux->file, addr, aux->read_bytes, aux->ofs);
            pml4_set_dirty(thread_current()->pml4, page->va, 0);
        }

        pml4_clear_page(thread_current()->pml4, page->va);
        addr += PGSIZE;
    }
}