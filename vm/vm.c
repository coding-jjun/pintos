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
 /*페이지가 초기화된 후에 그 유형을 알고 싶을 때 유용하다.
   완전히 구현이 된 상태*/
/* FIXME - anon_page, file_backed_page 완성 후 수정하기 */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT://페이지가 초기화되지 않은 상태라면
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
/*초기화 함수를 가진 pending 상태의 페이지 객체를 생성한다.
  페이지를 생성하려면 이 함수를 통하거나, vm_alloc_page를 통해 페이지를 직접 만들지 말고 생성
  실제 메모리에 할당되지 않은, 즉, 보류 중인(pending상태의) 페이지 객체를 의미한다.
  실제 물리 메모리에 로딩되지 않은 가상 메모리 페이지를 의미한다.*/
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable, vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check whether the upage is already occupied or not. */
	/*upage가 이미 사용중인지 아닌지 확인
	  특정 가상 메모리 주소가 이미 다른 페이지와 매핑되어 있는지 아닌지를 확인하는 작업
	  upage가 주소인거면 이미 사용중인건지 확인하는건가?*/
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		/* TODO: Insert the page into the spt. */
		struct page *page = (struct page *)malloc(sizeof(struct page));
		vm_initializer *initializer;
		
		
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
		//NOTE - init : lazy_load_segment -> page fault가 나면 lazy load segment
		uninit_new(page, upage, init, type, aux, initializer);
		if (type & VM_MARKER_1) { // mmap시 header page인 경우
			list_push_back(&thread_current()->head_list, &page->head_elem);
		}
		page->writable = writable;
		//NOTE - page 만들었으니까 insert
		return spt_insert_page(spt, page);
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
/*spt에서 va를 찾아 page를 반환한다.*/
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
	if(evict_start == NULL && !list_empty(&frame_table)){
		evict_start = list_begin(&frame_table);
	}

	struct frame *victim = NULL;
	struct thread *cur = thread_current();
	
	if(evict_start == NULL && !list_empty(&frame_table)){
    	evict_start = list_begin(&frame_table);
  	}

	struct list_elem *e = evict_start;
	int cnt = 0;

	for (e; e != list_end(&frame_table); e = list_next(e)) {
		victim = elem_to_frame(e);
		if (pml4_is_accessed(cur->pml4, victim->page->va)) {
			pml4_set_accessed(cur->pml4, victim->page->va, NOT_ACCESSED);
		} else {
			return victim;
		}
	}
	for (e = list_begin(&frame_table); e != list_end(&frame_table); e = list_next(e)) {
		victim = elem_to_frame(e);
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
/*palloc() 호출 및 프레임 획득. 만약 사용가능 한 페이지가 없다면, 페이지를 추방하고 반환한다.
  이 함수는 항상 유효한 주소를 반환한다. 즉, user pool 메모리가 가득차면, 이 함수는 사용
  가능한 메모리 영역을 얻기 위해 프레임을 추방한다*/
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
	if(vm_alloc_page(VM_ANON | VM_MARKER_0, addr, 1)){
        vm_claim_page(addr);
        thread_current()->stack_bottom -= PGSIZE;
    }
}

/* Handle the fault on write_protected page */
/*쓰기 보호된 페이지에 발생한 결함을 어떻게 처리할 것인지
  쓰기 보호된 페이지 : 해당 페이지에 대한 쓰기 연산이 제한되거나 금지된 페이지*/
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED, bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	if(is_kernel_vaddr(addr)){
		return false;
	}

	// stack pointer를 가져오는 방법(아까 thread 구조체에 저장했던 이유가 여기나옴)
	void *rsp_stack = is_kernel_vaddr(f->rsp) ? thread_current()->rsp_stack : f->rsp;

	if(not_present){ // 0: not-present page. 1: access rights violation.
			if(!vm_claim_page(addr)){
					if(rsp_stack - 8 <= addr && USER_STACK - 0x100000 <= addr && addr <= USER_STACK){
							vm_stack_growth(thread_current()->stack_bottom - PGSIZE);
							return true;
					}
			} else return true;
	}
	return false;
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
/*va에 할당된 페이지를 요구하다*/
bool
vm_claim_page (void *va UNUSED) {
	struct page *page;
	/* TODO: Fill this function */
	//만약 spt 테이블에서 page를 찾는다면 아직 물리 주소와 연결되지 않았다는 뜻이니까 do claim 호출
	page = spt_find_page(&thread_current()->spt, va);
	//페이지를 찾지 못했다면 -> 1. unmap되었거나 2. 초기화 되지 않았거나 사용되지 않았을 때 3. 유효하지 않거나 잘못된 주소일 때
	if (page == NULL) {
		return false;
	}
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
//물리 주소, 즉 프레임과 page 연결
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	//이미 물리 주소에 매핑이 된 페이지라면
    if(install_page(page->va, frame->kva, page->writable)){
		//디스크에 있는 페이지를 swap in으로 다시 물리 메모리에 로드해준다
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
//fork
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED, struct supplemental_page_table *src UNUSED) {
	/* TODO: copy the supplemental page table for fork */
	struct hash_iterator i;
	//부모 프로세스의 page 테이블을 fork
	hash_first(&i, &src->spt_hash);
	while(hash_next(&i)) {
		struct page *parent_page = h_elem_to_page(hash_cur(&i));
		// FIXME: 각 타입의 page type에 맞는 struct 완성시 조건문에 type 활용
		enum vm_type type = page_get_type(parent_page);
		void *upage = parent_page->va;
		bool writable = parent_page->writable;

		if (parent_page->uninit.type & VM_MARKER_0) {  // stack page인 경우
			setup_stack(&thread_current()->tf);
		} else if (parent_page->operations->type == VM_UNINIT) {  // uninit page인 경우 -> 아직 load 되지 않은 page
			vm_initializer *initializer = parent_page->uninit.init;
			struct lazy_load_info *aux = (struct lazy_load_info *)malloc(sizeof (struct lazy_load_info)); //aux를 malloc해주지 않으면 호출 함수로 인자를 넘긴 뒤로 사라져서 참조할 수 없어진다
			memcpy(aux, parent_page->uninit.aux, sizeof(struct lazy_load_info));//free해주거나 uninit이 먼저 destroy되어버리면 uninit.aux를 참조할 수 없기 때문에 memcpy해줌							
			if (!vm_alloc_page_with_initializer(type, upage, writable, initializer, aux)) { 
				return false;
			}
		} else {  // uninit page가 아닌 경우, page 할당 후 바로 mapping
			if (!vm_alloc_page(type, upage, writable) || !vm_claim_page(upage)) {
				return false;
			}
		}

		if (parent_page->operations->type != VM_UNINIT) { //스택일 경우에는 매핑이 되어있을
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
	struct list *h_list = &thread_current()->head_list;
	// struct list_elem *e;
	// if (!list_empty(h_list)) {
	// 	for (e = list_begin(h_list); e != list_end(h_list); e = list_next(e)) {
	// 		do_munmap(list_entry(e, struct page, head_elem));
	// 	}
	// }

	while (!list_empty(h_list)) {
		do_munmap(list_entry(list_front(h_list), struct page, head_elem)->va);
	}
	hash_clear(&spt->spt_hash, spt_destructor);
	// hash_destroy(&spt->spt_hash, spt_destructor);
}

void spt_destructor(struct hash_elem *e, void* aux){
    struct page *page = h_elem_to_page(e);
    vm_dealloc_page(page);
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