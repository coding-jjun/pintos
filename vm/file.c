/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "userprog/process.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

bool f_load_segment(struct file *file, off_t ofs, uint8_t *upage, uint32_t read_bytes, uint32_t zero_bytes, bool writable);
bool f_lazy_load_segment(struct page *page, void *aux);

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
  struct lazy_load_info *load_info = (struct lazy_load_info *)page->uninit.aux;

	page->operations = &file_ops;
	
	struct file_page *file_page = &page->file;

  file_page->file = load_info->file;
  file_page->ofs = load_info->ofs;
  file_page->read_bytes = load_info->read_bytes;
  file_page->zero_bytes = load_info->zero_bytes;

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

	// if (pml4_is_dirty(thread_current()->pml4, page->va)) {
  //   lock_acquire(inode_get_lock(file_get_inode(aux->file)));
  //   file_write_at(aux->file, page->va, aux->read_bytes, aux->ofs);
  //   lock_release(inode_get_lock(file_get_inode(aux->file)));
  //   pml4_set_dirty(thread_current()->pml4, page->va, 0);
	// }

	// pml4_clear_page(thread_current()->pml4, page->va);
	// file_close(aux->file);
}

bool f_load_segment(struct file *file, off_t ofs, uint8_t *upage, uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  bool header = true;

  while (read_bytes > 0 || zero_bytes > 0) {
    /* Do calculate how to fill this page.
     * We will read PAGE_READ_BYTES bytes from FILE
     * and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* TODO: Set up aux to pass information to the lazy_load_segment. */
    struct lazy_load_info *aux = malloc(sizeof(struct lazy_load_info));
    if (!aux) {
      return false;
    }

    aux->file = file;
    aux->ofs = ofs;
    aux->read_bytes = page_read_bytes;
    aux->zero_bytes = page_zero_bytes;
    aux->writable = writable;

    if (header) {  // header page인 경우
      if(!vm_alloc_page_with_initializer(VM_FILE | VM_MARKER_1, upage, writable, f_lazy_load_segment, aux)) {
        header = false;
        return false;
      }
      header = false;
    } else {  // header page가 아닌 경우
      if (!vm_alloc_page_with_initializer(VM_FILE, upage, writable, f_lazy_load_segment, aux)) {
        return false;
      }
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
    ofs += page_read_bytes;
  }
  return true;
}

bool f_lazy_load_segment(struct page *page, void *aux) {
  /* TODO: Load the segment from the file */
  /* TODO: This called when the first page fault occurs on address VA. */
  /* TODO: VA is available when calling this function. */
  struct lazy_load_info *load_info = (struct lazy_load_info *)aux;
  struct file *file = load_info->file;
  off_t ofs = load_info->ofs;
  size_t read_bytes = load_info->read_bytes;
  size_t zero_bytes = load_info->zero_bytes;
  free(load_info);
  
  void *upage = page->va;
  void *kpage = page->frame->kva;

  file_seek(file, ofs);
  if (file_read(file, kpage, read_bytes) != (int)read_bytes) {
    palloc_free_page(page->frame->kva);
    return false;
  }

  memset(kpage + read_bytes, 0, zero_bytes);
  return true;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset) {
  struct file *new_file = file_reopen(file);
	
	size_t read_bytes = length > file_length(file) ? file_length(file) : length;
	size_t zero_bytes = PGSIZE - read_bytes % PGSIZE;
	
	if (!f_load_segment(new_file, offset, addr, read_bytes, zero_bytes, writable)) {
		return NULL;
	}
	return addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
  struct thread *cur = thread_current();
  struct file *org_file;
  struct page *first_page = spt_find_page(&cur->spt, addr);
  if (first_page == NULL)
    return;

  list_remove(&first_page->head_elem);  // head_list에서 header page의 head_elem제거
  if (first_page->operations->type == VM_UNINIT) {
    org_file = ((struct lazy_load_info *)first_page->uninit.aux)->file;
  } else {
    org_file = first_page->file.file;
  }
  //spt 테이블 돌면서 unmap하려는 file과 관련된 page찾아서 munmap
  while (true) {
    struct page *page = spt_find_page(&cur->spt, addr);
    if (page == NULL)
      break;
    if (page->operations->type == VM_UNINIT) {
      if (((struct lazy_load_info *)page->uninit.aux)->file != org_file) {
        //page가 uninit인 경우인데, file과 관련된 page가 아닌 경우 break 
        break;
        
      }
    } else { //page가 file type일 경우
      if (page->file.file != org_file) { //page의 file type의 file이 현재 unmap시키려는 file이 아닌 경우->관련 없는 page인 경우
        break;
      }
      if (pml4_is_dirty(cur->pml4, page->va)) { //file과 관련있는 page인데 수정된 적이 있으면 수정된 사항을 덮어써야함 -> write하는 것과 다름없기 때문에 lock을 걸어줌
        // lock_acquire(inode_get_lock(file_get_inode(page->file.file)));
        file_write_at(page->file.file, page->va, page->file.read_bytes, page->file.ofs);
        // lock_release(inode_get_lock(file_get_inode(page->file.file)));
        pml4_set_dirty(cur->pml4, page->va, 0);
      }
      pml4_clear_page(cur->pml4, page->va);
    }
    addr += PGSIZE;
  }
  file_close(org_file);
}