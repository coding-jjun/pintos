/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

/* [ Add - LIB ] 2023.10.13 accessed bit 확인하기 위한 매크로 */
#include "threads/pte.h"

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

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
/* [ Add - LIB ] 2023.10.14 함수 구현중 */
bool vm_alloc_page_with_initializer (enum vm_type type, void *upage
									, bool writable, vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct thread *cur_t = thread_current();
	struct supplemental_page_table *spt = &cur_t -> spt;
	// struct page *page = (struct page*) upage;
	// 밑에서 page랑 upage 인자로 따로 받는거 보면 다른 변수인듯
	struct page *page = (struct page*) malloc(sizeof(struct page));
	struct lazy_load_info *info = (struct lazy_load_info *) aux;

	/* Check wheter the upage is already occupied or not. */
	//NOTE - pg_round_down를 해서 page의 시작 주소를 가르키게 해야하는 것 같음 ex) pg_round_down를(upage)
	if(spt_find_page(spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		
		switch(type){
			case VM_ANON:
				uninit_new(page, upage, init, type, aux, anon_initializer);
				break;
			case VM_FILE:
				uninit_new(page, upage, init, type, aux, file_backed_initializer);
				break;
			default:
				NOT_REACHED();
		}

		/* TODO: Insert the page into the spt. */
		spt_insert_page(spt, page);
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
/* Returns the page containing the given virtual address, or a null pointer if no such page exists. */
/* [ Upt - LIB ] 2023.10.13 gitbook help function 추가  */
struct page *spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	/* TODO: Fill this function. */
	struct page p;
	struct hash_elem *e;

	p.va = pg_round_down(va);
	e = hash_find(&spt->hash_table, &p.hash_elem);

	return e != NULL ? helem_to_page(e) : NULL;
}

/* Insert PAGE into spt with validation. */
/* [ Upt - LIB ] 2023.10.13 insert 추가  */
bool spt_insert_page (struct supplemental_page_table *spt UNUSED, struct page *page UNUSED) {
	/* TODO: Fill this function. */;
	page -> va = pg_round_down(page -> va);
	return hash_insert(&spt -> hash_table, &page -> hash_elem) ? true : false;
}

/* [ Upt - LIB ] 2023.10.13 delete 추가  */
void spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	// 필요한 코드인지 확신 x
	// hash_delete(&spt -> hash_table, &page -> hash_elem);    
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
/* [ Upt - Group ] 2023.10.13 vm_get_victim 함수 구현 */
static struct frame *vm_get_victim (void) {
	/* TODO: The policy for eviction is up to you. */
	if(list_empty(&frame_table)) return;

	struct frame *victim = NULL;
	struct thread *cur_t = thread_current();
	struct list_elem *e;

	for(e = list_begin(&frame_table); e != list_end(&frame_table); e = list_next(e)){
		victim = elem_to_frame(e);

		if(!pml4_is_accessed(cur_t -> pml4, victim -> kva)) {
			return victim;
		}
	}

	// clock algorithm
	if (victim == list_end(&frame_table)) {
		for(e = list_begin(&frame_table); e != list_end(&frame_table); e = list_next(e)) {
			victim = elem_to_frame(e);

			pml4_set_accessed(cur_t -> pml4, victim -> kva, NOT_ACCESSED);
		}
		return elem_to_frame(list_pop_front(&frame_table));
	}

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame * vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();

	/* TODO: swap out the victim and return the evicted frame. */
	// swap 관련 함수들 구현이 안되어있음
	swap_out(victim -> page);

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
/* [ Upt - Group ] 2023.10.13 vm_get_frame 함수 구현 */
static struct frame * vm_get_frame (void) {
	/* TODO: Fill this function. */
	struct frame *frame = (struct frame*)malloc(sizeof(struct frame));

	// 프레임 필드 값들 초기화 필요함
	frame -> kva = palloc_get_page(PAL_USER);
	// frame -> page

	if(!frame -> kva){
		vm_evict_frame();
		// 할당 실패시 vm_evict_frame 함수 호출해서 프레임 자리 만들어야함 (accessed bit 확인 후)
	}

	list_push_back(&frame_table, &frame -> frame_elem);

	// 페이지 할당 실패시 panic이 아니라 swap 해야하는데 구현 아직 안됨
	// PANIC("to do");

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
	
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = spt_find_page(spt, addr);

	if(page != NULL){
	  return vm_do_claim_page(page);
	} else {

		return NULL;
	}
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
	struct page *page = NULL;
	/* TODO: Fill this function */

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	/* REVIEW - 위에 코드가 매핑 확인 없는 코드라서 insert는 해야하는거 같은데 */
	// spt_insert_page(&thread_current() -> spt, page);

	return swap_in(page, frame->kva);
}

/* Initialize new supplemental page table */
/* [ Upt - LIB ] 2023.10.13 init 추가 */
void supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init(&spt -> hash_table, page_hash, page_less, NULL);
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

/* Returns a hash value for page p. */
/* [ Upt - LIB ] 2023.10.13 gitbook help function 추가 */
unsigned page_hash(const struct hash_elem *p_, void *aux UNUSED) {
  const struct page *p = helem_to_page(p_);

  return hash_bytes(&p->va, sizeof p->va);
}

/* [ Upt - LIB ] 2023.10.13 gitbook help function 추가 */
bool page_less(const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) {
  return helem_to_page(a_)->va < helem_to_page(b_)->va;
}

/* Returns true if page a precedes page b. */
// SECTION - Project 3 VM
/* [ Add - LIB ] 2023.10.13 변환 함수 추가 */
struct page* helem_to_page(const struct hash_elem *helem){
	return hash_entry(helem, struct page, hash_elem);
}

struct frame *elem_to_frame(struct list_elem *e){
	return list_entry(e, struct frame, frame_elem);
}
// !SECTION - Project 3 VM