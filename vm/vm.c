/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"

struct list_elem *evict_start;
struct list frame_table;


/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	list_init(&frame_table);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);
unsigned page_hash(const struct hash_elem *p_, void *aux UNUSED);
struct page *h_elem_to_page(struct hash_elem *h_elem);
struct frame *elem_to_frame(struct list_elem *elem);
bool page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED);
bool insert_page (struct hash *spt_hash, struct page *p);
bool delete_page (struct hash *spt_hash, struct page *p);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable, vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		/* TODO: Insert the page into the spt. */
		struct page *page = (struct page *)malloc(sizeof(struct page));
		typedef bool (*initializer_func) (struct page *, enum vm_type, void *);
		initializer_func initializer = NULL;
		switch (type) {
		case VM_ANON:
			initializer = anon_initializer;
			break;
		case VM_FILE:
			initializer = file_backed_initializer;
			break;
		default:
			break;
		}
		uninit_new(page, upage, init, type, aux, initializer);
		page->writable = writable;

		return spt_insert_page(spt, page);
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	/* TODO: Fill this function. */
	struct page *page  = (struct page *)malloc(sizeof(struct page));  // dummy page 생성
	struct hash_elem *e;
	page->va = pg_round_down(va);  // va가 가리키는 가상 page의 시작 포인트 반환
	e = hash_find(&spt->spt_hash, &page->h_elem);  // hash에서 hash_elem과 같은 요소를 검색해서 발견하면 발견한 hash elem 반환, 아니면 NULL반환
	// if (e == NULL) {
	// 	printf("hash_find 실패\n");
	// }
	free(page);  // hash elem을 찾기 위해 생성한 dummy page 해제

	return e != NULL ? h_elem_to_page(e) : NULL;
	// return e != NULL ? hash_entry(e, struct page, h_elem) : NULL;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED, struct page *page UNUSED) {
	// int succ = false;
	/* TODO: Fill this function. */
	return insert_page(&spt->spt_hash, page);
	// return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	struct thread *cur = thread_current();
	struct list_elem *e = evict_start;

	for (evict_start = e; evict_start != list_end(&frame_table); evict_start = list_next(evict_start)) {
		victim = elem_to_frame(evict_start);
		if (pml4_is_accessed(cur->pml4, victim->page->va)) {
			pml4_set_accessed(cur->pml4, victim->page->va, NOT_ACCESSED);
		} else {
			return victim;
		}
	}
	for (evict_start = list_begin(&frame_table); evict_start != e; evict_start = list_next(evict_start)) {
		victim = elem_to_frame(evict_start);
		if (pml4_is_accessed(cur->pml4, victim->page->va)) {
			pml4_set_accessed(cur->pml4, victim->page->va, NOT_ACCESSED);
		} else {
			return victim;
		}
	}

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */
	swap_out(victim->page);
	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	/* TODO: Fill this function. */
	struct frame *frame = (struct frame *)malloc(sizeof(struct frame));
	frame->kva = palloc_get_page(PAL_USER);

	if (frame->kva == NULL) {
		frame = vm_evict_frame();
		frame->page = NULL;
		return frame;
	}

	list_push_back(&frame_table, &frame->f_elem);

	frame->page = NULL;

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	page = spt_find_page(spt, addr);
	if (page == NULL) {
		return false;
	}

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page;
	/* TODO: Fill this function */
	page = spt_find_page(&thread_current()->spt, va);
	if (page == NULL) {
		return false;
	}
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
    if(install_page(page->va, frame->kva, page->writable)){
        return swap_in(page, frame->kva);
    }
    return false;
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init(&spt->spt_hash, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}

// SECTION - Project 3 VM
bool
install_page(void *upage, void *kpage, bool writable) {
  struct thread *t = thread_current();

  /* Verify that there's not already a page at that virtual
   * address, then map our page there. */
  return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}

unsigned
page_hash(const struct hash_elem *p_, void *aux UNUSED) {
	const struct page *p = h_elem_to_page(p_);
	return hash_bytes(&p->va, sizeof(p->va));
}

struct page *h_elem_to_page(struct hash_elem *hash_elem) {
	return hash_entry(hash_elem, struct page, h_elem);
}

struct frame *elem_to_frame(struct list_elem *elem) {
	return list_entry(elem, struct frame, f_elem);
}

bool
page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) {
	struct page *a = h_elem_to_page(a_);
	struct page *b = h_elem_to_page(b_);

	return a->va < b->va;
}

bool
insert_page (struct hash *spt_hash, struct page *p) {
	if (!hash_insert(spt_hash, &p->h_elem)) {
		return true;
	} else {
		return false;
	}
}

bool
delete_page (struct hash *spt_hash, struct page *p) {
	if (!hash_delete(spt_hash, &p->h_elem)) {
		return true;
	} else {
		return false;
	}
}
// !SECTION - Project 3 VM