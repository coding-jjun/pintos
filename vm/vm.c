/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"
#include "userprog/process.h"

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
/* FIXME - anon_page, file_backed_page 완성 후 수정하기 */
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
void spt_destructor(struct hash_elem *e, void *aux);
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
		switch (VM_TYPE(type)) {
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
	//NOTE - stack이니까 anon type
	if(vm_alloc_page(VM_ANON | VM_MARKER_0, addr, 1)){
		vm_claim_page(addr);
		thread_current()->stack_bottom -= PGSIZE;
	}
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
/**
 * @brief 
 * 
 * @param intr_frame f : 인터럽트나 예외가 발생했을 때 현재 CPU의 상태를 나타내는 구조체 
 * @param addr : 페이지 폴트가 발생한 가상 주소 
 * @param user : 페이지 폴트가 사용자 프로그램에서 발생했는지. true 면 사용자모드, false면 커널 모드 
 * @param write : 페이지 폴트가 발생한 연산이 쓰기 연산인지 
 * @param not_present : 해당 페이지가 메모리에 존재하지 않아서 페이지 폴트가 발생했는지 나타낸다. 
 * 						페이지가 물리 메모리에 없어서 발생했는지 나타낸다. 
 * @return true : 페이지 폴트를 성공적으로 처리했음을 의미. addr에 대한 페이지가 보조 페이지 테이블에 존재하며, 그 페이지를 물리적 메모리에
 * 				  올릴 수 있었음을 나타낸다.	   
 * @return false  : 페이지 폴트를 처리할 수 없었음. 주어진 가상 주소 addr에 대한 페이지가 보조 페이지 테이블에 존재하지 않거나, 페이지를 
 * 					물리적 메모리에 올리는 데 실패했음을 나타낸다.		
 */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	//NOTE - user space page fault여야 함
	if(is_kernel_vaddr(addr)){
		return false;
	}
	void *rsp_stack = is_kernel_vaddr(f->rsp) ? thread_current()->rsp_stack : f->rsp;
	//NOTE - not_present가 true이면 주어진 가상 주소 addr에 해당하는 페이지가 물리 메모리에 없다
	if(not_present){
		//NOTE - 주어진 주소에 대한 페이지를 물리 메모리에 할당하려고 시도
		if(!vm_claim_page(addr)){ //NOTE - 실패하면 페이지 폴트가 발생한 주소가 사용자 스택 영역에 있는지 확인
			if(rsp_stack-8 <= addr && USER_STACK - 0x100000 <= addr &&addr < USER_STACK){
				//NOTE - user stack의 특정 범위 내에 있으면, 스택 확장
				vm_stack_growth(thread_current()->stack_bottom - PGSIZE);
				return true;
			}//user 스택 영역에 없다면 false반환 페이지 폴트 처리
			return false;
		}
		//NOTE - 성공적으로 메모리 할당
		else{
			return true;
		}
	}
	//NOTE - 주어진 가상 주소 addr에 해당하는 페이지가 물리 메모리에 있다. 이 경우 페이지 폴트의 원인이 페이지가 물리 메모리에 없는 것이
	//		 아니다.(권한 오류 등)
	return false;
	//NOTE - 해당 페이지가 메모리에 올라갈 수 있는지 확인
	// return vm_do_claim_page (page);
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
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED, struct supplemental_page_table *src UNUSED) {
	/* TODO: copy the supplemental page table for fork */
	struct hash_iterator i;
	hash_first(&i, &src->spt_hash);
	while(hash_next(&i)) {
		struct page *parent_page = h_elem_to_page(hash_cur(&i));
		// FIXME: 각 타입의 page type에 맞는 struct 완성시 조건문에 type 활용
		enum vm_type type = page_get_type(parent_page);
		void *upage = parent_page->va;
		bool writable = parent_page->writable;

		if (parent_page->uninit.type & VM_MARKER_0) {  // stack page인 경우
			setup_stack(&thread_current()->tf);
		} else if (parent_page->operations->type == VM_UNINIT) {  // uninit page인 경우
			vm_initializer *initializer = parent_page->uninit.init;
			void *aux = parent_page->uninit.aux;
			if (!vm_alloc_page_with_initializer(type, upage, writable, initializer, aux)) {
				return false;
			}
		} else {  // uninit page가 아닌 경우, page 할당 후 바로 mapping
			if (!vm_alloc_page(type, upage, writable)) {
				return false;
			}
			if (!vm_claim_page(upage)) {
				return false;
			}
		}
		if (parent_page->operations->type != VM_UNINIT) {
			struct page *child_page = spt_find_page(dst, upage);
			memcpy(child_page->frame->kva, parent_page->frame->kva, PGSIZE);  // mapping된 frame에 부모의 frame 내용 복사
		}
	}
	return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	// struct hash_iterator i;
	// hash_first(&i, &spt->spt_hash);
	// while(hash_next(&i)){
	// 	struct page *page = hash_entry(hash_cur(&i), struct page, h_elem);

	// 	if(page->operations->type == VM_FILE){
	// 		do_munmap(page->va);
	// 	}
	// }
	// hash_destroy(&spt->spt_hash, spt_destructor);
	hash_clear(&spt->spt_hash, spt_destructor);
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

void 
spt_destructor(struct hash_elem *e, void *aux){
	const struct page *p = hash_entry(e, struct page, h_elem);
	free(p);
}
// !SECTION - Project 3 VM