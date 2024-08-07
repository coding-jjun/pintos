#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);
static bool lazy_load_segment(struct page *page, void *aux);

/**SECTION - Additional Decl*/
void argument_stack(int argc, char **argv, struct intr_frame *if_);
struct child_info *tid_to_child_info(tid_t child_tid);
bool setup_stack(struct intr_frame *if_);
/**!SECTION - Additional Decl*/

#endif /* userprog/process.h */
