/* vm.c: Generic interface for virtual memory objects. */

/*
	User Pool
	: 사용자 프로세스에서 사용할 수 있는 물리 메모리 frame의 pool

	User Page
	: 사용자 프로세스의 가상 주소 공간에 있는 특정 가상 페이지

	User Space
	: 사용자 프로세스의 전체 가상 주소 공간, User Page + Kernel Page
*/

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

/*
	Project 3: Frame 테이블 전역 변수 선언
	vm.c 파일의 모든 함수가 해당 frame_table을 공유
*/
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
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		/* TODO: Insert the page into the spt. */
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
/*
	spt_find_page: spt에서 va가 있는지를 찾는 함수, hash_find() 사용

	pg_round_down: 해당 va가 속해 있는 page의 시작 주소를 얻는 함수
	hash_find: Dummy page의 빈 hash_elem을 넣어주면, va에 맞는 hash_elem을 리턴해주는 함수 (hash_elem 갱신)

	hash_find가 NULL을 리턴할 수 있으므로, 리턴 시 NULL Check
*/
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page *page = (struct page *)malloc(sizeof(struct page));
	page->va = pg_round_down(va);

	struct hash_elem *e = hash_find(&spt->spt_hash, &page->hash_elem);

	free(page);

	return e == NULL ? NULL : hash_entry(e, struct page, hash_elem);
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED, struct page *page UNUSED) {
	return page_insert(&spt->spt_hash, page);
}	

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
/*
	vm_get_victim: Frame Table에서 해당 정보를 지우는 함수

	pml4에서 제거할 Page를 검색, 페이지 교체 알고리즘은 LRU (Least Recently Used)
	pml4_set_accessed() 사용하여 해당 Frame을 제거

	accessed bit을 0으로 설정하기 위해 pml4_set_accessed()에서 세번째 인자 0으로 넣어줌
*/
static struct frame *
vm_get_victim (void) {
	// 1) 현재 스레드의 pml4를 가져옴
	struct thread *curr = thread_current();
	uint64_t pml4 = &curr->pml4;

	// 2) Frame 테이블을 순회하며 victim을 찾고, accessed로 변경
	struct frame *victim = NULL;
	
	struct list_elem *e, *start;

	for (start = e; start != list_end(&frame_table); start = list_next(start)) {
		victim = list_entry(start, struct frame, frame_elem);
		if (pml4_is_accessed(curr->pml4, victim->page->va)) {
			pml4_set_accessed(curr->pml4, victim->page->va, 0);

		} else {
			return victim;
		}
	}

	for (start = list_begin(&frame_table); start != e; start = list_next(start)) {
		victim = list_entry(start, struct frame, frame_elem);
		if (pml4_is_accessed(curr->pml4, victim->page->va)) {
			pml4_set_accessed(curr->pml4, victim->page->va, 0);
			
		} else {
			return victim;
		}
	}
	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
/*
	vm_evict_frame: User pool과 Kernel pool에 사용 가능한 page가 없을 경우 Swap out을 진행하는 함수

	User pool -> Kenel pool -> Disk 순으로 free 페이지를 검색
	frame을 evict하는 함수이므로, NULL을 반환해주면 됨
*/
static struct frame *
vm_evict_frame (void) {
	// 1) vm_get_victim()을 사용하여 해당 정보를 Frame Table에서 삭제
	struct frame *victim = vm_get_victim ();

	// 2) Swap out: 해당 Page를 Swap Area로 내보냄, 즉 frame 공간을 disk로 내리는 것
	swap_out(victim->page);
	
	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
/*
	vm_get_frame: palloc_get_page 호출하여 새 Frame를 가져오는 함수
	성공적으로 가져오면 프레임을 할당하고 멤버를 초기화한 후 반환

	palloc_get_page: 하나의 free 페이지를 가져와 커널 가상 주소를 리턴하는 함수
	User pool -> Kernel pool
*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = (struct frame *)malloc(sizeof(struct frame));

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);

	frame->kva = palloc_get_page(PAL_USER);

	/*
		새 가상 주소 할당에 성공했다면, 해당 frame을 frame 테이블에 넣어주어야 함
		실패했다면, 기존 frame 중 하나를 비워주고 끝
	*/
	if (frame->kva != NULL) {
		list_push_back(&frame_table, &frame->frame_elem);
	} else {
		frame = vm_evict_frame();
	}

	frame->page = NULL;

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
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

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
/*
	vm_claim_page: 프레임을 페이지에 할당하는 함수
*/
bool
vm_claim_page (void *va UNUSED) {
	struct thread *curr = thread_current();
	struct page *page = spt_find_page(&curr->spt, va);

	if (page != NULL) {
		return vm_do_claim_page (page);

	} else {
		return false;
	}
}

/* Claim the PAGE and set up the mmu. */
/*
	vm_do_claim_page: 실제로 프레임을 페이지에 할당하는 함수

	Page는 User Page에 있고, Frame은 Kernel Page에 존재함
	install_page를 통해 가상 메모리 주소(Page)와 물리 메모리 주소(Frame)를 매핑
*/
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	if (install_page(page->va, frame->kva, page->writable)) {
		return swap_in (page, frame->kva);
		
	} else {
		return false;
	}
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init (&spt->spt_hash, page_hash, page_less, NULL);
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

// Helper Functions

/*
	hash_entry: 해당 hash_elem을 가지고 있는 page를 리턴하는 함수
	page_bytes: 해당 page의 가상 주소를 hashed index로 변환하는 함수
*/
unsigned page_hash (struct hash_elem *elem, void *aux UNUSED) {
	struct page *page = hash_entry(elem, struct page, hash_elem);

	return page_bytes(&page->va, sizeof(page->va));
}

/*
	page_less: 두 page의 주소값을 비교하여 왼쪽 값이 작으면 True 리턴하는 함수
*/
bool page_less (struct hash_elem *elema, struct hash_elem *elemb, void *aux UNUSED) {
	struct page *pagea = hash_entry(elema, struct page, hash_elem);
	struct page *pageb = hash_entry(elemb, struct page, hash_elem);

	return pagea->va < pageb->va;
}

/*
	page_insert: hash에 page를 삽입하는 함수, hash_insert() 사용
	해당 자리에 값이 있으면 삽입 실패
*/
bool page_insert (struct hash *hash, struct page *page) {
	if (!hash_insert(hash, &page->hash_elem)) {
		return true;
	} else {
		return false;
	}
}

/*
	page_delete: hash에 page를 삭제하는 함수 (hash, page), hash_delete() 사용
	해당 자리에 값이 없으면 삭제 실패
*/
bool page_delete (struct hash *hash, struct page *page) {
	if (hash_delete(hash, &page->hash_elem)) {
		return true;
	} else {
		return false;
	}
}

