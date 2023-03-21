#include "errno.h"
#include "globals.h"

#include "test/proctest.h"
#include "test/usertest.h"

#include "util/debug.h"
#include "util/printf.h"
#include "util/string.h"

#include "proc/kmutex.h"
#include "proc/kthread.h"
#include "proc/proc.h"
#include "proc/sched.h"

/*
 * Set up a testing function for the process to execute. 
*/
void *test_func(long arg1, void *arg2)
{
    proc_t *proc_as_arg = (proc_t *)arg2;
    test_assert(arg1 == proc_as_arg->p_pid, "Arguments are not set up correctly");
    test_assert(proc_as_arg->p_state == PROC_RUNNING, "Process state is not running");
    test_assert(list_empty(&proc_as_arg->p_children), "There should be no child processes");
    return NULL;
}

void test_termination()
{
    int num_procs_created = 0;
    proc_t *new_proc1 = proc_create("proc test 1");
    kthread_t *new_kthread1 = kthread_create(new_proc1, test_func, 2, new_proc1);
    num_procs_created++;
    sched_make_runnable(new_kthread1);

    int count = 0;
    int status;
    while (do_waitpid(-1, &status, 0) != -ECHILD)
    {
        test_assert(status == 0, "Returned status not set correctly");
        count++;
    }
    test_assert(count == num_procs_created,
                "Expected: %d, Actual: %d number of processes have been cleaned up\n", num_procs_created, count);
}

void *test_kill_all(long arg1, void *arg2)
{
    proc_kill_all();
    return NULL;
}

void *test_concurrent(long arg1, void *arg2)
{
    for (int i = 0; i < arg1; i++)
    {
        sched_yield();
    }
    return (void *)1000;
}

void *test_grandchild(long arg1, void *arg2)
{
    for (int i = 0; i < arg1; i++)
    {
        proc_t *new_proc = proc_create("grandchild test " + curproc->p_pid);
        kthread_t *new_kthread = kthread_create(new_proc, test_concurrent, 3, NULL);
        sched_make_runnable(new_kthread);
    }
    return NULL;
}

void test_proc()
{
    int num_child = 3;
    int num_grandchild = 6;
    int status;
    pid_t pids[] = {0, 0, 0};
    for (int i = 0; i < num_child; i++)
    {
        proc_t *new_proc = proc_create("child test " + i);
        pids[i] = new_proc->p_pid;
        kthread_t *new_kthread = kthread_create(new_proc, test_grandchild, 2, NULL);
        sched_make_runnable(new_kthread);
    }

    for (int i = 0; i < num_child; i++)
    {
        test_assert(do_waitpid(pids[i], &status, 0) == pids[i] && status == 0,
                    "Child process not exit correctly or returned status not set correctly");
    }

    test_assert(do_waitpid(0, NULL, 0) == -ENOTSUP, "Error check do_waitpid failed");
    test_assert(do_waitpid(0, NULL, 1) == -ENOTSUP, "Error check do_waitpid failed");
    test_assert(do_waitpid(-2, NULL, 0) == -ENOTSUP, "Error check do_waitpid failed");

    while (do_waitpid(-1, &status, 0) != -ECHILD)
    {
        test_assert(status == 1000, "Returned status not set correctly");
        num_grandchild -= 1;
    }
    test_assert(num_grandchild == 0,
                "Not all grandchild process has been adopted by init process or exited correctly");

    proc_t *kill_all_proc = proc_create("kill all test 1");
    kthread_t *kill_all_kthread = kthread_create(kill_all_proc, test_kill_all, 3, NULL);

    proc_t *new_proc = proc_create("child test " + num_child);
    kthread_t *new_kthread = kthread_create(new_proc, test_grandchild, 3, NULL);
    sched_make_runnable(new_kthread);

    sched_make_runnable(kill_all_kthread);
    test_assert(do_waitpid(kill_all_proc->p_pid, &status, 0) == kill_all_proc->p_pid && status == -1, "Kill all process not exit correctly or returned status not set correctly");

    test_assert(do_waitpid(-1, &status, 0) == new_proc->p_pid, "Wrong child returned");
    test_assert(status == 0 || status == -1, "Returned status not set correctly");
    for (int i = 0; i < 3; i++)
    {
        int ret = do_waitpid(-1, &status, 0);
        test_assert(ret != -ECHILD, "Number of child process mismatch");
        test_assert(status == 1000 || status == -1, "Returned status not set correctly");
    }
    test_assert(do_waitpid(-1, &status, 0) == -ECHILD, "Number of child process mismatch");
}

void *increment_counter(long arg1, void *arg2)
{
    kmutex_t *mtx = (kmutex_t *)arg1;
    int *ctr_ptr = (int *)arg2;
    kmutex_lock(mtx);
    int old_count = *ctr_ptr;
    sched_yield();
    *ctr_ptr = old_count + 1;
    kmutex_unlock(mtx);
    return NULL;
}

void test_mutex()
{
    int counter = 0;
    kmutex_t mtx;
    kmutex_init(&mtx);
    int num_proc = 1;
    for (int i = 0; i < num_proc; i++)
    {
        proc_t *new_proc = proc_create("mutex test " + i);
        kthread_t *new_kthread = kthread_create(new_proc, increment_counter, (long)&mtx, &counter);
        sched_make_runnable(new_kthread);
    }
    int status;
    while (do_waitpid(-1, &status, 0) != -ECHILD)
    {
        test_assert(status == 0, "Returned status not set correctly");
    }
    test_assert(counter == num_proc,
                "Expected: %d, Actual: %d number of times counter has been incremented\n", num_proc, counter);
}

long proctest_main(long arg1, void *arg2)
{
    dbg(DBG_TEST, "\nStarting Procs tests\n");
    test_init();
    test_termination();

    // Add more tests here!
    test_mutex();
    test_proc();

    test_fini();
    return 0;
}