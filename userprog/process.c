#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

#include "threads/synch.h" // P2

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);


// process_fork()에서 thread_create의 인자로 전달할 구조체 (P2)
struct fork_args {
	struct thread *parent;
	struct intr_frame *if_;
	struct semaphore fork_sema;
};

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
	current->is_user = true; // user process 여부 저장 (P2)
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	// P2
	// file_name에서 파일 이름을 제외한 인자를 모두 끊기
	char *p = file_name;
	while (*p != ' ' && *p != '\0') {
		p += 1;
	}
	*p = '\0';

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	/* Clone current thread to new thread.*/
	struct fork_args *fargs = palloc_get_page(0);
	if (fargs == NULL) {
		return TID_ERROR;
	}

	fargs->parent = thread_current();
	fargs->if_ = if_;

	sema_init(&fargs->fork_sema, 0); // fork가 끝날 때까지 부모를 대기시키는 sema

	tid_t tid = thread_create (name, PRI_DEFAULT, __do_fork, fargs);

	if (tid != TID_ERROR) {
		sema_down(&fargs->fork_sema); // __do_fork가 완료될 때까지 대기
		struct thread *t = thread_get_by_id(tid);
		if (t->p_tid == TID_ERROR) {
			// p_tid가 TID_ERROR인 경우 fork가 실패했음을 뜻함 (__do_fork 참조)
			sema_up(&t->reap_sema); // 프로세스 메모리 청소 허용
			tid = TID_ERROR;
		}
	}

	palloc_free_page(fargs);

	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (is_kern_pte(pte)) {
		return true;
	}

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page(PAL_USER); // 기존 pte에 대응되는 새로운 페이지 할당
	if (newpage == NULL) {
		// 할당 실패
		return false;
	}

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE); // 할당된 페이지로 데이터 복사
	writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct fork_args *fargs = (struct fork_args *) aux;

	struct thread *parent = fargs->parent;
	struct intr_frame *parent_if = fargs->if_;

	struct thread *current = thread_current ();
	struct intr_frame if_;
	
	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	// spt에 pml4를 연결 - vm.c supplemental_page_table_init() 참조
	current->spt.pml4 = current->pml4;
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	if (!thread_dup_file_list(parent, current)) { // parent의 file_list를 복사
		goto error;
	}
	process_init ();

	if_.R.rax = 0; // 반환 값을 0으로 설정
	
	/* Finally, switch to the newly created process. */
	if (succ) {
		sema_up(&fargs->fork_sema); // 대기중인 부모 프로세스를 깨운다
		do_iret (&if_);
	}
error:
	current->p_tid = TID_ERROR; // fork 실패 시 부모 tid를 -1로 설정
	sema_up(&fargs->fork_sema); // 대기중인 부모 프로세스를 깨운다
	thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();

	/* And then load the binary */
	success = load (file_name, &_if);

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t tid) {
	struct thread *t = thread_get_by_id(tid);

	if (t == NULL || t->p_tid != thread_current()->tid) {
		// tid 쓰레드가 없거나 자식 프로세스가 아님
		return -1;
	}
	t->p_tid = TID_ERROR; // 두 번 wait 불가하도록 부모를 삭제

	sema_down(&t->wait_sema); // 자식이 끝날 때까지 대기
	int child_exit = t->exit_status;
	sema_up(&t->reap_sema); // 자식 프로세스의 메모리 청소를 허용

	return child_exit;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */
	if (curr->is_user) {
		printf ("%s: exit(%d)\n", curr->name, curr->exit_status);
	}

	thread_clear_fd_page_list(curr);
	process_cleanup ();
	sema_up(&thread_current()->wait_sema); // 대기중인 부모를 깨움
	sema_down(&thread_current()->reap_sema); // 부모가 reap 할 때까지 대기
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

	if (curr->exe_file) {
		// 수정 방지를 위해 열어둔 파일 닫기
		file_allow_write(curr->exe_file);
		file_close(curr->exe_file);
	}

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
#ifdef VM
	// spt에 pml4를 연결 - vm.c supplemental_page_table_init() 참조
	t->spt.pml4 = t->pml4;
#endif
	process_activate (thread_current ());

	// P2
	// file_name에서 파일 이름을 제외한 인자를 모두 끊기
	char *p = file_name;
	while (*p != ' ' && *p != '\0') {
		p += 1;
	}
	char old_c = *p;
	*p = '\0';

	/* Open executable file. */
	lock_acquire(&file_lock);
	file = filesys_open (file_name);
	lock_release(&file_lock);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	// /* TODO: Your code goes here.
	//  * TODO: Implement argument passing (see project2/argument_passing.html). */

	char *rsp_start = (char *) if_->rsp;

	*p = old_c; // 끊어놓은 file_name을 원상복구
	char *start_p = file_name;

	p = start_p; // command line 시작
	while (*p != '\0') {
		p += 1;
	}
	char *end_p = p; // command line 끝

	int len;

	// push argv[][]
	p = end_p -1;
	while (p >= start_p) { // command line 끝부터 탐색
		while (*p == ' ') { // 공백을 모두 스킵
			p -= 1;
		}

		len = 0;
		for (; p >= start_p && *p != ' '; p -= 1) {
			// 공백이나 첫 문자가 나올 때까지 탐색
			len += 1;
		}

		if_->rsp -= len +1;
		strlcpy((char *) if_->rsp, p +1, len +1); // stack push
	}

	// word-align
	if_->rsp -= if_->rsp % sizeof(char*);

	// push argv[argc]
	if_->rsp -= sizeof(char*);
	*( (char **) if_->rsp) = NULL;

	// push argv[]
	// push argv[][]와 같지만, 단어의 길이만 push
	int argc = 0;
	char *argv = NULL;
	p = end_p -1;
	len = 0;
	while (p >= start_p) {
		while (*p == ' ') { // 공백을 모두 스킵
			p -= 1;
		}
		
		for (; p >= start_p && *p != ' '; p -= 1) {
			// 공백이나 첫 문자가 나올 때까지 탐색
			len += 1;
		}
		len += 1;

		if_->rsp -= sizeof(char*);
		*( (char **) if_->rsp) = rsp_start - len; // stack push

		argc += 1; // rdi에 입력할 argc
		argv = (char *) if_->rsp; // rsi에 입력할 argv
	}

	// push return address
	if_->rsp -= sizeof(void*);
	*( (void **) if_->rsp) = NULL;

	if_->R.rdi = argc;
	if_->R.rsi = argv;

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	if (file) {
		file_deny_write(file); // 실행중인 파일 수정을 막기 위해 파일을 열어둠
	}
	t->exe_file = file;
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
	struct uninit_page_args *upargs = (struct uninit_page_args*) aux;
	struct file *file = upargs->file;
	off_t ofs = upargs->ofs;
	uint32_t page_read_bytes = upargs->page_read_bytes;
	uint32_t page_zero_bytes = upargs->page_zero_bytes;

	ASSERT (page_read_bytes + page_zero_bytes == PGSIZE);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	/* Do calculate how to fill this page.
		* We will read PAGE_READ_BYTES bytes from FILE
		* and zero the final PAGE_ZERO_BYTES bytes. */

	/* Get a page of memory. */
	uint8_t *kpage = page->frame->kva;
	uint8_t *upage = page->va;
	ASSERT(kpage != NULL);

	/* Load this page. */
	if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
		printf("[DBG] lazy_load_segment(): file_read failed!\n");
		return false;
	}
	memset (kpage + page_read_bytes, 0, page_zero_bytes);

	free(upargs); // anon_initializer와 lazy_load_segment에서 사용이 모두 끝남
	return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	void *addr = upage;

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		// void *aux = NULL;
		struct uninit_page_args *upargs = malloc(sizeof(*upargs));
		upargs->file = file;
		upargs->ofs = ofs;
		upargs->page_read_bytes = page_read_bytes;
		upargs->page_zero_bytes = page_zero_bytes;
		upargs->is_stack = false;

		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, upargs))
		{
			printf("[DBG] load_segment(): vm_alloc_page_with_initializer failed!\n");
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	// bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */
	struct uninit_page_args *upargs = malloc(sizeof(*upargs));
	upargs->is_stack = true;

	if (!vm_alloc_page_with_initializer(VM_ANON, stack_bottom,
										true, NULL, upargs)) {
		printf("[DBG] setup_stack(): vm_alloc_page() failed!\n");
		return false;
	}
	
	if (!vm_claim_page(stack_bottom)) {
		printf("[DBG] setup_stack(): vm_claim_page() failed!\n");
		return false;
	}
	if_->rsp = USER_STACK;

	return true;
}
#endif /* VM */
