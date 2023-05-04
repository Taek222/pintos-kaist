#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "threads/palloc.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

void check_address (void *address);

void halt (void);
void exit (int status);
tid_t fork (const char *thread_name, struct intr_frame *f);
int exec (const char *cmd_line);
int wait (tid_t pid);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned size);
int write (int fd, const void *buffer, unsigned size);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);

struct file *find_file_by_fd(int fd);
int add_file_to_fdt(struct file *file);
void remove_file_from_fdt(int fd);

const int STDIN = 1;
const int STDOUT = 2;

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	// 파일 사용 관련 Lock 초기화
	lock_init(&file_lock);
}

/* The main system call interface */
/*
	syscall-nr.h의 enum 값을 사용하라고 가이드에 언급되어 있음

	1) intr_frame 구조체의 rax값을 사용, rax는 시스템 콜 번호
	2) 네번째 argument는 %rcx가 아니라 %r10
	3) %rdi, %rsi, %rdx, %r10, %r8, %r9의 순서로 argument가 전달됨
*/
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.

	switch (f->R.rax) {
		case SYS_HALT:
			halt();
			break;

		case SYS_EXIT:
			exit(f->R.rdi);
			break;

		case SYS_FORK:
			f->R.rax = fork(f->R.rdi, f);
			break;

		case SYS_EXEC:
			if (exec(f -> R.rdi) == -1){
                exit(-1);
            }
			break;

		case SYS_WAIT:
			f->R.rax = wait(f->R.rdi);
			break;

		case SYS_CREATE:
			f->R.rax = create(f->R.rdi, f->R.rsi);
			break;

		case SYS_REMOVE:
			f->R.rax = remove(f->R.rdi);
			break;

		case SYS_OPEN:
			f->R.rax = open(f->R.rdi);
			break;

		case SYS_FILESIZE:
			f->R.rax = filesize(f->R.rdi);
			break;

		case SYS_READ:
			f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
			break;

		case SYS_WRITE:
			f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;

		case SYS_SEEK:
			seek(f->R.rdi, f->R.rsi);
			break;

		case SYS_TELL:
			f->R.rax = tell(f->R.rdi);
			break;

		case SYS_CLOSE:
			close(f->R.rdi);
			break;

		default:
			exit(-1);
			break;
	}
}

/*
	Project 2: User Memory Access

	1) Null Pointer거나
	2) Kernel VM을 가르키거나 (=KERNEL_BASE보다 높은 주소)
	3) 매핑되지 않은 VM을 가르키거나 (할당되지 않은 주소)

	check_address: 해당 주소가 올바른 접근인지 확인, 올바르지 않은 주소일 경우 프로세스 종료
*/
void check_address (void *address) {
	struct thread *curr = thread_current();

	if (address == NULL | !is_user_vaddr(address) | pml4_get_page(curr->pml4, address) == NULL) {
		exit(-1);
	}
}

/*
	Poject 2: System Calls

	시스템 콜 및 시스템 콜 핸들러 구현
	프로세스가 유저모드일 때 하지 못하는 일을 커널에게 요청
*/

// 핀토스를 종료시킴, init.h의 power_off() 사용
void halt (void) {
	power_off();
}

// 현재 사용자 프로그램을 종료하여 커널에 상태를 반환
void exit (int status) {
	struct thread *curr = thread_current();
	curr->exit_status = status;

	printf ("%s: exit(%d)\n", thread_name(), status);

	thread_exit();
}

// 현재 프로세스의 복제본인 새 프로세스를 thread_name이라는 이름으로 생성
tid_t fork (const char *thread_name, struct intr_frame *f) {
	return process_fork(thread_name, f);
}

/*
	작업이 성공하면 절대 반환되지 않음
	프로그램을 로드하거나 실행할 수 없는 경우, 프로세스 종료 상태 -1
*/
// 주어진 인수를 전달하여 현재 프로세스를 cmd_line에 지정된 이름의 실행 파일로 변경
int exec (const char *cmd_line) {
	check_address(cmd_line);

	char *cl_copy = palloc_get_page(PAL_ZERO);
	if (cl_copy == NULL) {
		exit(-1);
	}

	strlcpy (cl_copy, cmd_line, strlen(cmd_line) + 1);

	if (process_exec(cl_copy) == -1) {
		return -1;
	}

	NOT_REACHED();
	return 0;
}

// 자식 프로세스 pid를 기다렸다가 자식의 종료 상태를 검색, process.c의 process_wait() 사용
int wait (tid_t pid) {
	return process_wait(pid);
}

// 초기 initial_size 바이트 크기의 file이라는 새 파일을 생성, filesys.c의 filesys_create() 사용
bool create (const char *file, unsigned initial_size) {
	check_address(file);

	return filesys_create(file, initial_size);
}

// file이라는 파일을 삭제, filesys.c의 filesys_remove() 사용
bool remove (const char *file) {
	check_address(file);

	return filesys_remove(file);
}

/*
	fd가 음수가 아닌 정수를 반환하거나 파일을 열 수 없는 경우 -1 리턴
	fd는 자식 프로세스에 의해 상속됨, 단일 파일을 두번 이상 열면 열 때마다 새 fd가 리턴됨
	별도의 close 호출이 있어야 하며, 파일 위치 공유 안 함
*/
// file이라는 파일을 오픈, filesys.c의 filesys_open() 사용, file.c의 file_close() 사용
int open (const char *file) {
	check_address(file);

	lock_acquire(&file_lock);

	struct file *open_file = filesys_open(file);

	if (open_file == NULL) {
		return -1;
	}

	int fd = add_file_to_fdt(open_file);

	// fdt가 꽉 차 더이상 추가할 수 없는 상태
	if (fd == -1) {
		file_close(open_file);
	}

	lock_release(&file_lock);

	return fd;
}

// fd로 열린 파일의 크기(바이트 단위)를 리턴, file.c의 file_length() 사용
int filesize (int fd) {
	struct file *file = find_file_by_fd(fd);

	if (file == NULL) {
		return -1;
	}

	return file_length(file);
}

/*
	Lock을 사용하여 파일에 동시 접근 허용하지 않음, FD를 통해 파일 검색

	1) fd가 0이면 키보드 입력을 버퍼에 저장한 후 저장된 크기를 리턴, input_getc() 사용
	2) fd가 0이 아니면 파일의 데이터를 size만큼 저장 후 저장된 크기를 리턴
*/
// fd로 열린 파일을 버퍼(바이트 단위)로 읽음, input.c의 input_getc() 사용, file.c의 file_read() 사용
int read (int fd, void *buffer, unsigned size) {
	check_address(buffer);

	struct file *file = find_file_by_fd(fd);
	if (file == NULL) {
		return -1;
	}

	int count;
	unsigned char *value = buffer;
	if (file == STDIN) {
		for (int i = 0; i < size; i++) {
			char key = input_getc();
			*value++ = key;

			if (key == "\0") {
				break;
			}
		}
		count = size;

	} else if (file == STDOUT) {
		return -1;

	} else {
		lock_acquire(&file_lock);
        count = file_read(file, buffer, size);
        lock_release(&file_lock);
	}

	return count;
}

// fd에 size 크기(바이트 단위)의 buffer 값을 씀, file.c의 file_write() 사용
int write (int fd, const void *buffer, unsigned size) {
	check_address(buffer);

	struct file *file = find_file_by_fd(fd);
	if (file == NULL) {
		return -1;
	}

	int count;
	if (file == STDIN) {
		return -1;

	} else if (file == STDOUT) {
		putbuf(buffer, size);
		count = size;
	
	} else {
		lock_acquire(&file_lock);
        count = file_write(file, buffer, size);
        lock_release(&file_lock);
	}

	return count;
}

// fd에서 읽거나 쓸 다음 바이트를 파일 시작 부분부터 바이트 단위로 표시되는 위치로 변경, file.c의 file_seek() 사용
void seek (int fd, unsigned position) {
	struct file *file = find_file_by_fd(fd);

	if (fd <= 1 || file == NULL) {
		return;
	}

	file_seek(file, position);
}

// fd에서 읽거나 쓸 다음 바이트의 위치를 바이트 단위로 리턴, file.c의 file_tell() 사용
unsigned tell (int fd) {
	struct file *file = find_file_by_fd(fd);

	if (fd <= 1 || file == NULL) {
		return;
	}

	return file_tell(file);
}

void close (int fd) {
	struct file *file = find_file_by_fd(fd);

	if (fd <= 1 || file == NULL) {
		return;
	}

	remove_file_from_fdt(fd);
}

// ↓ System Call 함수들이 필요로 하는 함수들

// find_file_by_fd: 현재 스레드가 읽고 있는 fd를 리턴하는 함수
struct file *find_file_by_fd (int fd) {
    if (fd < 0 || fd >= FDCOUNT_LIMIT){
        return NULL;
    }

    struct thread *curr = thread_current();

    return curr->fd_table[fd];
}

// add_file_to_fdt: 테이블을 조회하여 넣을 수 있는 곳을 찾고, 해당 파일을 추가
int add_file_to_fdt(struct file *file) {
    struct thread *curr = thread_current();
    struct file **fdt = curr->fd_table;

	// fd 테이블의 앞부터 순차적으로 검색, fd_index는 fdt 인덱스의 마지막 값
    while (curr->fd_index < FDCOUNT_LIMIT && fdt[curr->fd_index]) {
        curr->fd_index++;
    }

	// fd 테이블이 꽉 차 더이상 추가할 수 없는 상태라면 -1 리턴
    if (curr->fd_index >= FDCOUNT_LIMIT) {
		return -1;
	}

    fdt[curr->fd_index] = file;

    return curr->fd_index;
}

// remove_file_from_fdt: 현재 스레드가 열고 있는 파일을 삭제
void remove_file_from_fdt(int fd) {
    if (fd < 0 || fd >= FDCOUNT_LIMIT) {
        return;
    }

    struct thread *cur = thread_current();
    cur->fd_table[fd] = NULL;
}